#include "DX12NativeAcceleration.h"
#include "DX12GpuRetirement.h"
#include "Utils/TracyZones.h"
#include "VideoMemoryBudget.h"

#include <algorithm>
#include <cstring>
#include <directx/d3dx12.h>
#include <DirectXPackedVector.h>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace DX12Native
{
	namespace
	{
		constexpr uint32_t kMaximumInstances = (1u << 24u) - 1u;
		struct AllocationDeferred
		{};

	}

	AccelerationScene::~AccelerationScene()
	{
		if (!completion)
			return;
		const uint64_t completed = completion->GetCompletedValue();
		if (completed == UINT64_MAX)
			return;  // Device removed; submitted GPU work cannot continue.

		std::array<Build, 2> retiring{};
		uint64_t lastSignalled = 0;
		bool needsDrain = false;
		bool inFlight = false;
		for (size_t i = 0; i < builds.size(); ++i) {
			const uint64_t fenceValue = builds[i].fence;
			if (!fenceValue || (fenceValue != UINT64_MAX && fenceValue <= completed))
				continue;
			if (fenceValue == UINT64_MAX)
				needsDrain = true;  // ExecuteCommandLists succeeded, Signal failed.
			else
				lastSignalled = std::max(lastSignalled, fenceValue);
			retiring[i] = std::move(builds[i]);
			inFlight = true;
		}
		if (!inFlight)
			return;

		// A tail marker tracks any ExecuteCommandLists whose original Signal failed.
		// If even the rescue marker fails, retain until device removal.
		std::vector<FenceDependency> dependencies;
		if (lastSignalled)
			dependencies.push_back(FencePoint(completion.get(), lastSignalled));
		if (needsDrain) {
			winrt::com_ptr<ID3D12Fence> drain;
			if (device && backend.commandQueue &&
				SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(drain.put()))) &&
				SUCCEEDED(backend.commandQueue->Signal(drain.get(), 1))) {
				drain->SetName(L"DX12NativeScene::AccelerationRetirementFence");
				dependencies.push_back(FencePoint(drain.get(), 1));
			} else {
				dependencies.push_back(FencePoint(completion.get(), UINT64_MAX));
			}
		}
		winrt::com_ptr<ID3D12Device> retainedDevice;
		if (device)
			retainedDevice.copy_from(device.get());
		RetireAfterFences(std::pair{ backend.commandQueue, std::move(retiring) }, std::move(retainedDevice), std::move(dependencies));
	}

	bool AccelerationScene::Initialize(const GraphicsBackend& a_backend)
	{
		if (completion)
			return backend.d3d12Device.get() == a_backend.d3d12Device.get() &&
			       backend.commandQueue.get() == a_backend.commandQueue.get();
		if (!a_backend.d3d12Device || !a_backend.commandQueue)
			return false;
		try {
			backend = a_backend;
			DX::ThrowIfFailed(backend.d3d12Device->QueryInterface(IID_PPV_ARGS(device.put())));
			D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
			DX::ThrowIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)));
			if (options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
				return false;
			DX::ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(completion.put())));
			completion->SetName(L"DX12NativeScene::AccelerationFence");
			failure.clear();
			return true;
		} catch (const std::exception& error) {
			failure = error.what();
			return false;
		}
	}

	winrt::com_ptr<ID3D12Resource> AccelerationScene::Buffer(uint64_t a_size, D3D12_HEAP_TYPE a_heap, D3D12_RESOURCE_STATES a_state, const wchar_t* a_name) const
	{
		if (!a_size)
			throw std::runtime_error("Zero-size native acceleration allocation");
		const auto heap = CD3DX12_HEAP_PROPERTIES(a_heap);
		const auto flags = a_heap == D3D12_HEAP_TYPE_DEFAULT ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
		const auto desc = CD3DX12_RESOURCE_DESC::Buffer(a_size, flags);
		const uint64_t bytes = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
		if (!bytes || bytes == UINT64_MAX)
			throw std::runtime_error("Invalid native acceleration allocation");
		if (bytes > videoBudget.Query(device.get(), 0).effectiveBytes)
			throw AllocationDeferred{};
		winrt::com_ptr<ID3D12Resource> resource;
		DX::ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, a_state, nullptr, IID_PPV_ARGS(resource.put())));
		resource->SetName(a_name);
		return resource;
	}

	void AccelerationScene::Begin(Build& a_build)
	{
		a_build.generation = generation;
		DX::ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(a_build.allocator.put())));
		DX::ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a_build.allocator.get(), nullptr, IID_PPV_ARGS(a_build.commands.put())));
	}

	void AccelerationScene::Submit(Build& a_build)
	{
		ZoneScopedN("DX12::Acceleration::Submit");
		if (!GraphicsBackend::CheckNativeDeviceHealth(device.get(), backend.d3d11Device.get()))
			DX::ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
		DX::ThrowIfFailed(a_build.commands->Close());
		ID3D12CommandList* commands[] = { a_build.commands.get() };
		backend.commandQueue->ExecuteCommandLists(1, commands);
		const uint64_t signalValue = nextFence + 1;
		const HRESULT signalResult = backend.commandQueue->Signal(completion.get(), signalValue);
		if (FAILED(signalResult)) {
			// A queued submission has no valid completion marker. Retain Build
			// and drain the direct queue during owned asynchronous retirement.
			a_build.fence = UINT64_MAX;
			DX::ThrowIfFailed(signalResult);
		}
		a_build.fence = signalValue;
		nextFence = signalValue;
	}

	bool AccelerationScene::BuildBottom(Build& a_build, const std::shared_ptr<const DX12::Shape>& a_shape)
	{
		ZoneScopedN("DX12::Acceleration::BuildBLAS");
		if (!a_shape || !a_shape->identity || a_shape->positions.empty() ||
			a_shape->indices.empty() || a_shape->indices.size() % 3u ||
			a_shape->positions.size() > UINT32_MAX || a_shape->indices.size() > UINT32_MAX)
			return false;
		const uint64_t vertexBytes = uint64_t(a_shape->positions.size()) * sizeof(float3);
		const uint64_t indexBytes = uint64_t(a_shape->indices.size()) * sizeof(uint32_t);
		a_build.sourceUpload = Buffer(vertexBytes + indexBytes, D3D12_HEAP_TYPE_UPLOAD,
			D3D12_RESOURCE_STATE_GENERIC_READ, L"DX12NativeScene::RigidGeometryUpload");
		void* mapped = nullptr;
		const D3D12_RANGE noRead{ 0, 0 };
		DX::ThrowIfFailed(a_build.sourceUpload->Map(0, &noRead, &mapped));
		std::memcpy(mapped, a_shape->positions.data(), size_t(vertexBytes));
		std::memcpy(static_cast<uint8_t*>(mapped) + vertexBytes, a_shape->indices.data(), size_t(indexBytes));
		a_build.sourceUpload->Unmap(0, nullptr);

		D3D12_RAYTRACING_GEOMETRY_DESC triangle{};
		triangle.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		triangle.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		triangle.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		triangle.Triangles.VertexCount = UINT(a_shape->positions.size());
		triangle.Triangles.VertexBuffer = { a_build.sourceUpload->GetGPUVirtualAddress(), sizeof(float3) };
		triangle.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
		triangle.Triangles.IndexCount = UINT(a_shape->indices.size());
		triangle.Triangles.IndexBuffer = a_build.sourceUpload->GetGPUVirtualAddress() + vertexBytes;

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.NumDescs = 1;
		inputs.pGeometryDescs = &triangle;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
		device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
		if (!info.ResultDataMaxSizeInBytes || !info.ScratchDataSizeInBytes)
			return false;

		a_build.bottom = std::make_shared<BottomLevel>();
		a_build.bottom->shape = a_shape;
		a_build.bottom->acceleration = Buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"DX12NativeScene::BLAS");
		a_build.scratch = Buffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"DX12NativeScene::BLASScratch");
		Begin(a_build);
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
		build.Inputs = inputs;
		build.DestAccelerationStructureData = a_build.bottom->acceleration->GetGPUVirtualAddress();
		build.ScratchAccelerationStructureData = a_build.scratch->GetGPUVirtualAddress();
		a_build.commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
		const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(a_build.bottom->acceleration.get());
		a_build.commands->ResourceBarrier(1, &barrier);
		Submit(a_build);
		return true;
	}

	void AccelerationScene::PrepareShadingGeometry(Build& a_build, TopLevel& top)
	{
		const auto& scene = *top.inputs;
		if (!scene.materials || scene.geometry.empty() ||
			scene.geometry.size() > UINT32_MAX / sizeof(ShadingGeometryBinding))
			return;
		std::vector<ShadingGeometryBinding> table(scene.geometry.size());
		std::vector<std::vector<float>> attributes(scene.geometry.size());
		struct CopyRange { const void* data; uint64_t offset, bytes; };
		std::vector<CopyRange> copies;
		uint64_t bytes = uint64_t(table.size()) * sizeof(ShadingGeometryBinding);
		const uint64_t budget = videoBudget.Query(device.get(), 0).effectiveBytes / 2;
		if (bytes >= budget)
			return;
		const auto append = [&](const void* data, uint64_t length) {
			const auto offset = uint32_t(bytes);
			copies.push_back({ data, bytes, length });
			bytes += length;
			return offset;
		};
		uint32_t ready = 0;
		for (size_t i = 0; i < scene.geometry.size(); ++i) {
			const auto& shape = scene.geometry[i];
			if (!shape || !shape->material || scene.materials->candidateIndices[i] == UINT32_MAX)
				continue;
			const auto& source = *shape->material;
			const auto validOffset = [&](uint32_t offset) {
				return source.vertexStride >= 16 && offset >= 16 && offset <= source.vertexStride - 4;
			};
			if (source.vertexCount != shape->positions.size() ||
				uint64_t(source.vertexCount) * source.vertexStride != source.vertexBytes.size() ||
				!validOffset(source.uvOffset) || !validOffset(source.normalOffset) ||
				(source.tangentOffset != UINT32_MAX && !validOffset(source.tangentOffset)) ||
				(source.colorOffset != UINT32_MAX && !validOffset(source.colorOffset)) ||
				shape->indices.empty() || shape->indices.size() > UINT32_MAX)
				continue;
			const uint32_t stride = source.tangentOffset == UINT32_MAX ? 36u : 60u;
			const uint64_t positionBytes = uint64_t(shape->positions.size()) * sizeof(float3);
			const uint64_t indexBytes = uint64_t(shape->indices.size()) * sizeof(uint32_t);
			const uint64_t attributeBytes = uint64_t(source.vertexCount) * stride;
			const uint64_t total = positionBytes + indexBytes + attributeBytes;
			if (total >= UINT32_MAX - bytes || total > budget - bytes)
				continue;
			auto& decoded = attributes[i];
			decoded.reserve(size_t(attributeBytes / sizeof(float)));
			const auto sn = [](uint8_t value) { return float(value) * (2.0f / 255.0f) - 1.0f; };
			for (uint32_t j = 0; j < source.vertexCount; ++j) {
				const auto* vertex = source.vertexBytes.data() + size_t(j) * source.vertexStride;
				uint16_t uv[2];
				float bx;
				std::memcpy(uv, vertex + source.uvOffset, sizeof(uv));
				std::memcpy(&bx, vertex + 12, sizeof(bx));
				const auto* n = vertex + source.normalOffset;
				const auto* t = source.tangentOffset == UINT32_MAX ? nullptr : vertex + source.tangentOffset;
				const auto* color = source.colorOffset == UINT32_MAX ? nullptr : vertex + source.colorOffset;
				const float u = DirectX::PackedVector::XMConvertHalfToFloat(uv[0]);
				const float v = DirectX::PackedVector::XMConvertHalfToFloat(uv[1]);
				if ((t && !std::isfinite(bx)) || !std::isfinite(u) || !std::isfinite(v)) {
					decoded.clear();
					break;
				}
				decoded.insert(decoded.end(), { sn(n[0]), sn(n[1]), sn(n[2]), u, v,
					color ? float(color[0])/255.0f : 1.0f, color ? float(color[1])/255.0f : 1.0f,
					color ? float(color[2])/255.0f : 1.0f, color ? float(color[3])/255.0f : 1.0f });
				if (t)
					decoded.insert(decoded.end(), { sn(t[0]), sn(t[1]), sn(t[2]), bx, sn(n[3]), sn(t[3]) });
			}
			if (decoded.empty())
				continue;
			table[i] = { append(shape->positions.data(), positionBytes), append(shape->indices.data(), indexBytes),
				append(decoded.data(), attributeBytes), stride, source.vertexCount, uint32_t(shape->indices.size()), uint32_t(sizeof(float3)), 0 };
			++ready;
		}
		if (!ready) {
			top.shadingFailure = "No supported native shading geometry";
			return;
		}
		const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
		const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
		if (allocation == UINT64_MAX || allocation > videoBudget.Query(device.get(), 0).effectiveBytes / 2) {
			top.shadingFailure = "Native shading geometry deferred by DXGI budget";
			return;
		}
		auto resident = std::make_shared<ShadingGeometry>();
		resident->bytes = bytes;
		resident->revision = top.geometryRevision;
		resident->instanceCount = uint32_t(table.size());
		resident->readyInstances = ready;
		const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		DX::ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(resident->buffer.put())));
		resident->buffer->SetName(L"DX12NativeScene::FullNativeShadingGeometry");
		auto staging = Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"DX12NativeScene::FullNativeShadingUpload");
		void* mapped = nullptr;
		const D3D12_RANGE noRead{};
		DX::ThrowIfFailed(staging->Map(0, &noRead, &mapped));
		std::memcpy(mapped, table.data(), table.size() * sizeof(table.front()));
		for (const auto& range : copies)
			std::memcpy(static_cast<uint8_t*>(mapped) + size_t(range.offset), range.data, size_t(range.bytes));
		staging->Unmap(0, nullptr);
		a_build.shadingUpload = std::move(staging);
		top.shadingGeometry = std::move(resident);
	}

	bool AccelerationScene::BuildTop(Build& a_build)
	{
		ZoneScopedN("DX12::Acceleration::BuildTLAS");
		if (!target || target->geometry.empty() || target->geometry.size() > kMaximumInstances)
			return false;
		std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
		auto top = std::make_shared<TopLevel>();
		top->inputs = target;
		top->geometryRevision = targetRevision;
		top->scope = targetScope;
		top->producerFence = completion;
		instances.reserve(target->geometry.size());
		top->bottoms.reserve(target->geometry.size());

		for (const auto& input : target->geometry) {
			const auto found = bottoms.find(input->identity);
			if (found == bottoms.end() || found->second->shape.get() != input.get())
				return false;
			D3D12_RAYTRACING_INSTANCE_DESC desc{};
			std::memcpy(desc.Transform, input->transform.data(), sizeof(desc.Transform));
			desc.InstanceID = UINT(instances.size());
			desc.InstanceMask = 0xFF;
			desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
			desc.AccelerationStructure = found->second->acceleration->GetGPUVirtualAddress();
			instances.push_back(desc);
			top->bottoms.push_back(found->second);
			top->blasBytes += found->second->acceleration->GetDesc().Width;
		}

		const uint64_t bytes = uint64_t(instances.size()) * sizeof(instances.front());
		a_build.sourceUpload = Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD,
			D3D12_RESOURCE_STATE_GENERIC_READ, L"DX12NativeScene::InstanceUpload");
		void* mapped = nullptr;
		const D3D12_RANGE noRead{ 0, 0 };
		DX::ThrowIfFailed(a_build.sourceUpload->Map(0, &noRead, &mapped));
		std::memcpy(mapped, instances.data(), size_t(bytes));
		a_build.sourceUpload->Unmap(0, nullptr);

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		inputs.NumDescs = UINT(instances.size());
		inputs.InstanceDescs = a_build.sourceUpload->GetGPUVirtualAddress();
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
		device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
		if (!info.ResultDataMaxSizeInBytes || !info.ScratchDataSizeInBytes)
			return false;

		top->tlasBytes = info.ResultDataMaxSizeInBytes;
		top->acceleration = Buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"DX12NativeScene::TLAS");
		a_build.scratch = Buffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"DX12NativeScene::TLASScratch");
		// Only Full Native publishes material candidates. Shadow modes do not enter this path.
		if (target->materials && !target->materials->recipes.empty()) {
			try {
				PrepareShadingGeometry(a_build, *top);
			} catch (const AllocationDeferred&) {
				top->shadingFailure = "Native shading geometry allocation deferred";
			} catch (const std::exception& error) {
				top->shadingFailure = error.what();
			}
			try {
				top->materialEvaluation = materialResidency.Prepare(device.get(), *target->materials, *target, a_build.materialUploads);
				if (!top->materialEvaluation)
					top->materialFailure = "Material allocation deferred or unavailable";
			} catch (const std::exception& error) {
				top->materialFailure = error.what();
			}
			if (!top->materialEvaluation)
				a_build.materialUploads.clear();
		}
		a_build.top = std::move(top);
		Begin(a_build);
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
		build.Inputs = inputs;
		build.DestAccelerationStructureData = a_build.top->acceleration->GetGPUVirtualAddress();
		build.ScratchAccelerationStructureData = a_build.scratch->GetGPUVirtualAddress();
		a_build.commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
		const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(a_build.top->acceleration.get());
		a_build.commands->ResourceBarrier(1, &barrier);
		if (const auto& shading = a_build.top->shadingGeometry) {
			a_build.commands->CopyBufferRegion(shading->buffer.get(), 0, a_build.shadingUpload.get(), 0, shading->bytes);
			const auto shadingBarrier = CD3DX12_RESOURCE_BARRIER::Transition(shading->buffer.get(),
				D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			a_build.commands->ResourceBarrier(1, &shadingBarrier);
		}
		if (a_build.top->materialEvaluation)
			NativeMaterialEvaluation::RecordUploads(a_build.commands.get(), a_build.materialUploads);
		Submit(a_build);
		a_build.top->producerValue = a_build.fence;
		return true;
	}

	bool AccelerationScene::HasPendingBottom(uint64_t a_identity, const DX12::Shape* a_shape) const
	{
		return std::ranges::any_of(builds, [&](const auto& build) {
			return build.generation == generation && build.bottom &&
			       build.bottom->shape->identity == a_identity && build.bottom->shape.get() == a_shape;
		});
	}

	void AccelerationScene::Retire()
	{
		ZoneScopedN("DX12::Acceleration::Retire");
		const uint64_t done = completion->GetCompletedValue();
		if (done == UINT64_MAX) {
			failure = "DX12 native acceleration device removed before completion";
			return;
		}
		for (auto& build : builds) {
			if (!build.fence || build.fence > done)
				continue;
			if (build.generation == generation) {
				if (build.bottom)
					bottoms[build.bottom->shape->identity] = build.bottom;
				if (build.top) {
					materialResidency.PublishCompleted(build.top->materialEvaluation);
					published = build.top;
				}
			}
			build = {};
		}
	}

	void AccelerationScene::Reset()
	{
		++generation;
		target.reset();
		bottoms.clear();
		materialResidency.Reset();
		published.reset();
		targetRevision = 0;
		targetScope = 0;
	}

	void AccelerationScene::Advance(std::shared_ptr<const SceneInputs> a_inputs)
	{
		budgetDeferred = false;
		if (!completion || !failure.empty())
			return;
		Retire();
		if (!failure.empty())
			return;
		if (!a_inputs || !a_inputs->residency || !a_inputs->residency->requested.front().scope)
			return;
		const uint32_t scope = a_inputs->residency->requested.front().scope;
		if (!target || targetRevision != a_inputs->geometryRevision || targetScope != scope) {
			if (targetScope && targetScope != scope)
				Reset();
			else {
				++generation;
				published.reset();
			}
			target = std::move(a_inputs);
			targetRevision = target->geometryRevision;
			targetScope = scope;
			std::unordered_set<uint64_t> wanted;
			for (const auto& input : target->geometry)
				if (input)
					wanted.insert(input->identity);
			std::erase_if(bottoms, [&](const auto& pair) {
				return !wanted.contains(pair.first);
			});
		}
		if (!target)
			return;
		if (published && published->geometryRevision == targetRevision && published->scope == targetScope)
			return;

		Build* available = nullptr;
		for (auto& build : builds)
			if (!build.allocator) {
				available = &build;
				break;
			}
		if (!available)
			return;
		try {
			for (const auto& input : target->geometry) {
				if (!input)
					continue;
				const auto found = bottoms.find(input->identity);
				if (found != bottoms.end() && found->second->shape.get() == input.get())
					continue;
				if (HasPendingBottom(input->identity, input.get()))
					return;
				if (!BuildBottom(*available, input))
					*available = {};
				return;
			}
			if (!std::ranges::any_of(builds, [&](const auto& build) {
					return build.generation == generation && build.top != nullptr;
				})) {
				if (!BuildTop(*available))
					*available = {};
			}
		} catch (const AllocationDeferred&) {
			budgetDeferred = true;
			if (!available->fence)
				*available = {};
		} catch (const std::exception& error) {
			failure = error.what();
			if (!available->fence)
				*available = {};
		}
	}

}

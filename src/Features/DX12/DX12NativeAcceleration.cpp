#include "DX12NativeAcceleration.h"
#include "VideoMemoryBudget.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <directx/d3dx12.h>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace DX12Native
{
	namespace
	{
		constexpr uint32_t kMaximumInstances = (1u << 24u) - 1u;

		void Check(HRESULT result)
		{
			if (FAILED(result))
				throw std::runtime_error("DX12 native acceleration resource or command failure");
		}
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

		// Retain the resources in an owning retirement task, not leaked allocations.
		// A tail marker drains queue submissions for which Signal() originally failed.
		std::thread([pending = std::move(retiring), producer = completion,
						queue = backend.commandQueue, retainedDevice = device,
						lastSignalled, needsDrain]() mutable {
			(void)pending;  // The capture deliberately owns the queued GPU resources.
			winrt::com_ptr<ID3D12Fence> drainFence;
			bool drainScheduled = !needsDrain;
			while (true) {
				const uint64_t done = producer->GetCompletedValue();
				if (done == UINT64_MAX)
					break;  // Device removal permits releasing the resources.
				if (!drainScheduled && retainedDevice && queue) {
					if (!drainFence)
						(void)retainedDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(drainFence.put()));
					if (drainFence && SUCCEEDED(queue->Signal(drainFence.get(), 1)))
						drainScheduled = true;
				}
				if (done >= lastSignalled && drainScheduled &&
					(!needsDrain || drainFence->GetCompletedValue() >= 1))
					break;
				if (!retainedDevice || FAILED(retainedDevice->GetDeviceRemovedReason()))
					break;
				std::this_thread::sleep_for(std::chrono::milliseconds(8));
			}
		}).detach();
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
			Check(backend.d3d12Device->QueryInterface(IID_PPV_ARGS(device.put())));
			D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
			Check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)));
			if (options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
				return false;
			Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(completion.put())));
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
		winrt::com_ptr<ID3D12Resource> resource;
		Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, a_state, nullptr, IID_PPV_ARGS(resource.put())));
		resource->SetName(a_name);
		return resource;
	}

	void AccelerationScene::Begin(Build& a_build)
	{
		a_build.generation = generation;
		Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(a_build.allocator.put())));
		Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a_build.allocator.get(), nullptr, IID_PPV_ARGS(a_build.commands.put())));
	}

	void AccelerationScene::Submit(Build& a_build)
	{
		Check(a_build.commands->Close());
		ID3D12CommandList* commands[] = { a_build.commands.get() };
		backend.commandQueue->ExecuteCommandLists(1, commands);
		const uint64_t signalValue = nextFence + 1;
		const HRESULT signalResult = backend.commandQueue->Signal(completion.get(), signalValue);
		if (FAILED(signalResult)) {
			// A queued submission has no valid completion marker. Retain Build
			// and drain the direct queue during owned asynchronous retirement.
			a_build.fence = UINT64_MAX;
			Check(signalResult);
		}
		a_build.fence = signalValue;
		nextFence = signalValue;
	}

	bool AccelerationScene::BuildBottom(Build& a_build, const std::shared_ptr<const DX12::Shape>& a_shape)
	{
		if (!a_shape || !a_shape->identity || a_shape->Positions().empty() ||
			a_shape->Indices().empty() || a_shape->Indices().size() % 3u ||
			a_shape->Positions().size() > UINT32_MAX || a_shape->Indices().size() > UINT32_MAX)
			return false;
		const uint64_t vertexBytes = uint64_t(a_shape->Positions().size()) * sizeof(float3);
		const uint64_t indexBytes = uint64_t(a_shape->Indices().size()) * sizeof(uint32_t);
		a_build.sourceUpload = Buffer(vertexBytes + indexBytes, D3D12_HEAP_TYPE_UPLOAD,
			D3D12_RESOURCE_STATE_GENERIC_READ, L"DX12NativeScene::RigidGeometryUpload");
		void* mapped = nullptr;
		const D3D12_RANGE noRead{ 0, 0 };
		Check(a_build.sourceUpload->Map(0, &noRead, &mapped));
		std::memcpy(mapped, a_shape->Positions().data(), size_t(vertexBytes));
		std::memcpy(static_cast<uint8_t*>(mapped) + vertexBytes, a_shape->Indices().data(), size_t(indexBytes));
		a_build.sourceUpload->Unmap(0, nullptr);

		D3D12_RAYTRACING_GEOMETRY_DESC triangle{};
		triangle.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		triangle.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		triangle.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		triangle.Triangles.VertexCount = UINT(a_shape->Positions().size());
		triangle.Triangles.VertexBuffer = { a_build.sourceUpload->GetGPUVirtualAddress(), sizeof(float3) };
		triangle.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
		triangle.Triangles.IndexCount = UINT(a_shape->Indices().size());
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

	void AccelerationScene::PrepareShadingGeometry(Build& a_build, TopLevel& a_top)
	{
		// Native-only, optional shader geometry. Leave the completed shadow TLAS
		// usable if residency is unavailable. One immutable buffer per published
		// generation, with offsets shared by all identical resident assets.
		if (!a_top.preparedMaterials || !a_top.inputs)
			return;
		const auto& geometry = a_top.inputs->geometry;
		if (geometry.size() > UINT32_MAX / sizeof(ShadingGeometryBinding))
			return;
		std::vector<ShadingGeometryBinding> table(geometry.size());
		struct CopyRange { const void* data; uint64_t offset, bytes; };
		std::vector<CopyRange> copies;
		struct Offsets { uint32_t position, index; };
		std::unordered_map<const void*, Offsets> meshes;
		std::unordered_map<const DX12::PreparedShading*, uint32_t> attributes;
		uint64_t bytes = uint64_t(table.size()) * sizeof(ShadingGeometryBinding);
		uint32_t ready = 0;
		const auto append = [&](const void* data, uint64_t length) -> uint32_t {
			if (!length || bytes >= UINT32_MAX || length >= UINT32_MAX - bytes)
				throw std::runtime_error("Native shading geometry address space exhausted");
			const auto offset = uint32_t(bytes);
			copies.push_back({ data, bytes, length });
			bytes += length;
			return offset;
		};
		for (size_t i = 0; i < geometry.size(); ++i) {
			const auto& shape = geometry[i].shape;
			if (!shape || !shape->shading || shape->shading->stride != 60)
				continue;
			const auto& positions = shape->Positions();
			const auto& indices = shape->Indices();
			const auto& shading = *shape->shading;
			if (positions.empty() || indices.empty() || positions.size() > UINT32_MAX || indices.size() > UINT32_MAX ||
				uint64_t(shading.attributes.size()) * sizeof(float) != uint64_t(positions.size()) * shading.stride)
				continue;
			// Only stage geometry with a preliminary material identity.
			const auto& material = a_top.preparedMaterials->bindings[i];
			if (material.identity[0] == 0 && material.identity[1] == 0)
				continue;
			auto mesh = meshes.find(shape->GeometryKey());
			if (mesh == meshes.end()) {
				const Offsets offsets{
					append(positions.data(), uint64_t(positions.size()) * sizeof(float3)),
					append(indices.data(), uint64_t(indices.size()) * sizeof(uint32_t)) };
				mesh = meshes.emplace(shape->GeometryKey(), offsets).first;
			}
			auto attr = attributes.find(&shading);
			if (attr == attributes.end()) {
				const auto offset = append(shading.attributes.data(), uint64_t(shading.attributes.size()) * sizeof(float));
				attr = attributes.emplace(&shading, offset).first;
			}
			table[i] = { mesh->second.position, mesh->second.index, attr->second, shading.stride,
				uint32_t(positions.size()), uint32_t(indices.size()), uint32_t(sizeof(float3)), 0 };
			++ready;
		}
		if (!ready) {
			a_top.shadingFailure = "No supported shading geometry";
			return;
		}
		const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
		const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
		VideoMemoryBudget budget;
		const auto available = budget.Query(device.get(), 0);
		// Conservatively allow for both staging and resident allocation, even on
		// UMA. Never assume headroom when DXGI telemetry is absent.
		if (allocation == UINT64_MAX || !available.dxgiAvailable || allocation > available.effectiveBytes / 2) {
			a_top.shadingFailure = "Native shading geometry deferred by DXGI budget";
			return;
		}
		auto resident = std::make_shared<ShadingGeometry>();
		resident->bytes = bytes;
		resident->revision = a_top.geometryRevision;
		resident->instanceCount = uint32_t(table.size());
		resident->readyInstances = ready;
		const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(resident->buffer.put())));
		resident->buffer->SetName(L"DX12NativeScene::ShadingGeometry");
		auto staging = Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
			L"DX12NativeScene::ShadingGeometryUpload");
		void* mapped = nullptr;
		const D3D12_RANGE noRead{ 0, 0 };
		Check(staging->Map(0, &noRead, &mapped));
		std::memcpy(mapped, table.data(), table.size() * sizeof(table.front()));
		for (const auto& range : copies)
			std::memcpy(static_cast<uint8_t*>(mapped) + size_t(range.offset), range.data, size_t(range.bytes));
		staging->Unmap(0, nullptr);
		a_build.shadingUpload = std::move(staging);
		a_top.shadingGeometry = std::move(resident);
	}

	bool AccelerationScene::BuildTop(Build& a_build)
	{
		if (!target || target->geometry.empty() || target->geometry.size() > kMaximumInstances)
			return false;
		std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
		auto top = std::make_shared<TopLevel>();
		top->inputs = target;
		if (target->residency && target->residency->fullNative)
			top->preparedMaterials = CompilePreparedMaterials(*target);
		top->geometryRevision = targetRevision;
		top->scope = targetScope;
		top->producerFence = completion;
		instances.reserve(target->geometry.size());
		top->bottoms.reserve(target->geometry.size());

		for (const auto& input : target->geometry) {
			const auto found = bottoms.find(input.shape->GeometryKey());
			if (found == bottoms.end() || found->second->shape->GeometryKey() != input.shape->GeometryKey())
				return false;
			D3D12_RAYTRACING_INSTANCE_DESC desc{};
			std::memcpy(desc.Transform, input.shape->transform.data(), sizeof(desc.Transform));
			desc.InstanceID = UINT(instances.size());
			desc.InstanceMask = 0xFF;
			desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
			desc.AccelerationStructure = found->second->acceleration->GetGPUVirtualAddress();
			instances.push_back(desc);
			top->bottoms.push_back(found->second);
		}

		const uint64_t bytes = uint64_t(instances.size()) * sizeof(instances.front());
		a_build.sourceUpload = Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD,
			D3D12_RESOURCE_STATE_GENERIC_READ, L"DX12NativeScene::InstanceUpload");
		void* mapped = nullptr;
		const D3D12_RANGE noRead{ 0, 0 };
		Check(a_build.sourceUpload->Map(0, &noRead, &mapped));
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

		top->acceleration = Buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"DX12NativeScene::TLAS");
		a_build.scratch = Buffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"DX12NativeScene::TLASScratch");
		try {
			PrepareShadingGeometry(a_build, *top);
		} catch (const std::exception& error) {
			top->shadingFailure = error.what();
			top->shadingGeometry.reset();
			a_build.shadingUpload = nullptr;
		}
		if (top->shadingGeometry && top->preparedMaterials) {
			try {
				a_build.materialEvaluation = NativeMaterialEvaluation::Prepare(device.get(), *top->preparedMaterials, *top->inputs,
					materialTextures, a_build.materialUploads);
				if (!a_build.materialEvaluation)
					top->materialFailure = "NativePBR evaluation deferred or no eligible material sources";
			} catch (const std::exception& error) {
				top->materialFailure = error.what();
				a_build.materialEvaluation.reset();
				a_build.materialUploads.clear();
			}
		}
		a_build.top = std::move(top);
		Begin(a_build);
		if (const auto& shading = a_build.top->shadingGeometry) {
			a_build.commands->CopyBufferRegion(shading->buffer.get(), 0, a_build.shadingUpload.get(), 0, shading->bytes);
			const auto shadingBarrier = CD3DX12_RESOURCE_BARRIER::Transition(shading->buffer.get(),
				D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			a_build.commands->ResourceBarrier(1, &shadingBarrier);
			if (a_build.materialEvaluation)
				a_build.materialEvaluation->Record(a_build.commands.get(), shading->buffer.get(), a_build.materialUploads);
		}
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
		build.Inputs = inputs;
		build.DestAccelerationStructureData = a_build.top->acceleration->GetGPUVirtualAddress();
		build.ScratchAccelerationStructureData = a_build.scratch->GetGPUVirtualAddress();
		a_build.commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
		const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(a_build.top->acceleration.get());
		a_build.commands->ResourceBarrier(1, &barrier);
		Submit(a_build);
		a_build.top->producerValue = a_build.fence;
		return true;
	}

	bool AccelerationScene::HasPendingBottom(const void* a_key) const
	{
		return std::ranges::any_of(builds, [&](const auto& build) {
			return build.generation == generation && build.bottom &&
			       build.bottom->shape->GeometryKey() == a_key;
		});
	}

	void AccelerationScene::Retire()
	{
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
					bottoms[build.bottom->shape->GeometryKey()] = build.bottom;
				if (build.top) {
					if (build.materialEvaluation) {
						build.materialEvaluation->Complete();
						materialTextures = build.materialEvaluation->textures;
						build.top->materialEvaluation = std::move(build.materialEvaluation);
					} else {
						materialTextures.clear();
					}
					published = build.top;
				}
			}
			build = {};
		}
		std::erase_if(consumers, [](const Consumer& consumer) {
			return consumer.fence->GetCompletedValue() >= consumer.value;
		});
	}

	void AccelerationScene::Reset()
	{
		++generation;
		target.reset();
		bottoms.clear();
		materialTextures.clear();
		published.reset();
		targetRevision = 0;
		targetScope = 0;
	}

	void AccelerationScene::Advance(std::shared_ptr<const SceneInputs> a_inputs)
	{
		if (!completion || !failure.empty())
			return;
		Retire();
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
			if (!target->residency->fullNative)
				materialTextures.clear();
			targetRevision = target->geometryRevision;
			targetScope = scope;
			std::unordered_set<const void*> wanted;
			for (const auto& input : target->geometry)
				if (input.shape)
					wanted.insert(input.shape->GeometryKey());
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
				if (!input.shape)
					continue;
				const auto found = bottoms.find(input.shape->GeometryKey());
				if (found != bottoms.end() && found->second->shape->GeometryKey() == input.shape->GeometryKey())
					continue;
				if (HasPendingBottom(input.shape->GeometryKey()))
					return;
				if (!BuildBottom(*available, input.shape))
					*available = {};
				return;
			}
			if (!std::ranges::any_of(builds, [&](const auto& build) {
					return build.generation == generation && build.top != nullptr;
				})) {
				if (!BuildTop(*available))
					*available = {};
			}
		} catch (const std::exception& error) {
			failure = error.what();
			if (!available->fence)
				*available = {};
		}
	}

	bool AccelerationScene::RetainThrough(const std::shared_ptr<const TopLevel>& a_scene, ID3D12Fence* a_fence, uint64_t a_value)
	{
		if (!a_scene || !a_fence || !a_value)
			return false;
		Consumer consumer;
		consumer.scene = a_scene;
		consumer.fence.copy_from(a_fence);
		consumer.value = a_value;
		consumers.push_back(std::move(consumer));
		return true;
	}
}

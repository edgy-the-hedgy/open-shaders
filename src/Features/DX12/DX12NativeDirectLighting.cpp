#include "DX12NativeDirectLighting.h"
#include "DX12GpuRetirement.h"
#include "GpuPass.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <directx/d3dx12.h>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace DX12Native
{
	namespace
	{
		using Eval = NativeMaterialEvaluation;
		constexpr uint32_t kLightingStride = 16;
		struct Constants
		{
			Matrix projectionInverse, viewInverse;
			float4 positionAdjust;
			uint32_t width, height, materialPitch, lightCount;
			float nearLightRadius;
			uint32_t nearLightExclusion, padding0 = 0, padding1 = 0;
		};
		static_assert(sizeof(Constants) <= 256);

		void WriteConstants(ID3D12Resource* resource, const Constants& constants)
		{
			void* mapped = nullptr;
			const D3D12_RANGE noRead{};
			Eval::Check(resource->Map(0, &noRead, &mapped));
			std::memcpy(mapped, &constants, sizeof(constants));
			resource->Unmap(0, nullptr);
		}
	}

	DirectLightingTrace::~DirectLightingTrace()
	{
		current.reset();
		for (auto& slot : slots)
			if (slot && !GpuFenceRetired(slot->readyFence.get(), slot->readyValue))
				RetireWhenGpuDone(std::move(slot), owner.d3d12Device, [](const DirectLightingFrame& frame) {
					return GpuFenceRetired(frame.readyFence.get(), frame.readyValue);
				});
	}

	void DirectLightingTrace::Poll(uint32_t frame)
	{
		if (current && current->frame != frame) current.reset();
		for (auto& slot : slots)
			if (slot && slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue)) {
				slot->material.reset();
				slot->lights = nullptr;
			}
	}

	void DirectLightingTrace::Initialize(const GraphicsBackend& backend)
	{
		if (pipeline) {
			if (owner.d3d12Device.get() != backend.d3d12Device.get() || owner.commandQueue.get() != backend.commandQueue.get())
				throw std::runtime_error("Direct lighting device changed; restart required");
			return;
		}
		auto* device = backend.d3d12Device.get();
		std::ifstream file("Data/Shaders/NativeComposite/DirectLighting.dxil", std::ios::binary | std::ios::ate);
		if (!file || file.tellg() <= 0 || file.tellg() > 16 * 1024 * 1024)
			throw std::runtime_error("DirectLighting.dxil unavailable");
		std::vector<char> shader(static_cast<size_t>(file.tellg()));
		file.seekg(0);
		if (!file.read(shader.data(), std::streamsize(shader.size())))
			throw std::runtime_error("Direct lighting DXIL read failed");
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);
		CD3DX12_ROOT_PARAMETER parameters[6];
		parameters[0].InitAsShaderResourceView(0);
		parameters[1].InitAsShaderResourceView(1);
		parameters[2].InitAsShaderResourceView(2);
		parameters[3].InitAsDescriptorTable(1, &range);
		parameters[4].InitAsUnorderedAccessView(0);
		parameters[5].InitAsConstantBufferView(0);
		const CD3DX12_ROOT_SIGNATURE_DESC signature(6, parameters);
		winrt::com_ptr<ID3DBlob> blob, errors;
		winrt::com_ptr<ID3D12RootSignature> newRoot;
		winrt::com_ptr<ID3D12PipelineState> newPipeline;
		winrt::com_ptr<ID3D12Fence> newFence;
		Eval::Check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put()));
		Eval::Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(newRoot.put())));
		D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
		pso.pRootSignature = newRoot.get();
		pso.CS = { shader.data(), shader.size() };
		Eval::Check(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(newPipeline.put())));
		Eval::Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(newFence.put())));
		owner = backend;
		root = std::move(newRoot);
		pipeline = std::move(newPipeline);
		completion = std::move(newFence);
	}

	void DirectLightingTrace::Dispatch(const GraphicsBackend& backend,
		std::shared_ptr<const NativeScreenProbe::MaterialFrame> material,
		std::span<const DirectPointLight> lights, bool inputsAvailable, const ShadowTraceControls& controls)
	{
		CS_GPU_PASS("NativeLighting::Direct");
		if (!material || (current && current->frame == material->frame)) return;
		Poll(material->frame);
		dispatchedLights = 0;
		if (!inputsAvailable) { failure = "Waiting for native light inputs"; return; }
		if (!material->samples || !material->producerFence || !material->producerValue ||
			!material->scene || !material->scene->acceleration || !material->raster ||
			!material->raster->depth || !material->raster->depth->resource ||
			!material->raster->cameraValid || material->raster->frame != material->frame ||
			material->scene->geometryRevision != material->geometryRevision ||
			material->scene->scope != material->scope || material->sampleStride != 32 ||
			material->width != controls.viewportWidth || material->height != controls.viewportHeight)
			return;
		const uint32_t width = material->width, height = material->height;
		const uint64_t bytes = uint64_t(width) * height * kLightingStride;
		if (!bytes || bytes > UINT32_MAX) return;

		std::shared_ptr<DirectLightingFrame>* selected = nullptr;
		bool submitted = false;
		try {
			Initialize(backend);
			if (completion->GetCompletedValue() == UINT64_MAX)
				throw std::runtime_error("Direct lighting device removed");
			for (auto& slot : slots) {
				if (!slot || (slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue))) {
					selected = &slot;
					if (slot && slot->width == width && slot->height == height) break;
				}
			}
			if (!selected) return;
			auto& slot = *selected;
			auto* device = backend.d3d12Device.get();
			const bool fresh = !slot || slot->width != width || slot->height != height;
			if (fresh) {
				const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				const uint64_t allocation = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
				if (allocation == UINT64_MAX || allocation > UINT64_MAX - 1024 * 1024)
					throw std::runtime_error("Direct lighting allocation unsupported");
				const uint64_t required = allocation + 1024 * 1024;
				uint64_t tracked = 0;
				for (const auto& held : slots) if (held) tracked += held->allocationBytes;
				const auto budget = videoBudget.Query(device, tracked);
				if (!budget.dxgiAvailable || required > budget.effectiveBytes || tracked > budget.effectiveBytes - required)
					throw std::runtime_error("Direct lighting deferred by DXGI budget");
				auto replacement = std::make_shared<DirectLightingFrame>();
				replacement->allocationBytes = required;
				replacement->width = width; replacement->height = height; replacement->rowPitch = width * kLightingStride;
				replacement->samples = Eval::Buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				replacement->samples->SetName(L"NativeLighting::DiffuseSpecular");
				replacement->camera = Eval::Buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
				D3D12_DESCRIPTOR_HEAP_DESC heap{};
				heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				heap.NumDescriptors = 1; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				Eval::Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(replacement->descriptors.put())));
				Eval::Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(replacement->allocator.put())));
				Eval::Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, replacement->allocator.get(), pipeline.get(), IID_PPV_ARGS(replacement->commands.put())));
				slot = std::move(replacement);
			} else {
				Eval::Check(slot->allocator->Reset());
				Eval::Check(slot->commands->Reset(slot->allocator.get(), pipeline.get()));
			}
			slot->readyFence = completion; slot->readyValue = UINT64_MAX;
			slot->root = root; slot->pipeline = pipeline;
			slot->frame = material->frame; slot->material = std::move(material);
			const auto& raster = *slot->material->raster;
			std::vector<std::array<float, 4>> records;
			records.reserve(std::min(lights.size(), size_t(kMaxShadowLights)) * 3);
			for (const auto& light : lights) {
				if (records.size() / 3 >= kMaxShadowLights) break;
				if (!std::isfinite(light.radius) || light.radius <= 0 ||
					!std::isfinite(light.position.x) || !std::isfinite(light.position.y) || !std::isfinite(light.position.z) ||
					!std::isfinite(light.radiance.x) || !std::isfinite(light.radiance.y) || !std::isfinite(light.radiance.z) ||
					!std::isfinite(light.sourceRadius) || !std::isfinite(light.fadeZone)) continue;
				records.push_back({ light.position.x, light.position.y, light.position.z, light.radius });
				records.push_back({ light.radiance.x, light.radiance.y, light.radiance.z, 0 });
				records.push_back({ light.sourceRadius, light.fadeZone, light.inverseSquare ? 1.0f : 0.0f, 0 });
			}
			const uint32_t lightCount = uint32_t(records.size() / 3);
			if (records.empty()) records.resize(3);
			slot->lights = Eval::Table(device, records);
			const Constants constants{ raster.camera.CameraProjInverse, raster.camera.CameraViewInverse,
				raster.camera.CameraPosAdjust, width, height, slot->material->rowPitch, lightCount,
				controls.nearLightRadius, controls.nearLightExclusion ? 1u : 0u };
			WriteConstants(slot->camera.get(), constants);
			D3D12_SHADER_RESOURCE_VIEW_DESC depth{};
			depth.Format = DXGI_FORMAT_R32_FLOAT; depth.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			depth.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; depth.Texture2D.MipLevels = 1;
			auto* depthResource = raster.depth->resource.get();
			device->CreateShaderResourceView(depthResource, &depth, slot->descriptors->GetCPUDescriptorHandleForHeapStart());
			auto* command = slot->commands.get();
			ID3D12DescriptorHeap* heaps[]{ slot->descriptors.get() };
			command->SetDescriptorHeaps(1, heaps);
			command->SetComputeRootSignature(root.get());
			command->SetComputeRootShaderResourceView(0, slot->material->scene->acceleration->GetGPUVirtualAddress());
			command->SetComputeRootShaderResourceView(1, slot->material->samples->GetGPUVirtualAddress());
			command->SetComputeRootShaderResourceView(2, slot->lights->GetGPUVirtualAddress());
			command->SetComputeRootDescriptorTable(3, slot->descriptors->GetGPUDescriptorHandleForHeapStart());
			command->SetComputeRootUnorderedAccessView(4, slot->samples->GetGPUVirtualAddress());
			command->SetComputeRootConstantBufferView(5, slot->camera->GetGPUVirtualAddress());
			const auto depthStart = CD3DX12_RESOURCE_BARRIER::Transition(depthResource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			command->ResourceBarrier(1, &depthStart);
			if (!fresh) {
				const auto outputStart = CD3DX12_RESOURCE_BARRIER::Transition(slot->samples.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				command->ResourceBarrier(1, &outputStart);
			}
			command->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
			const std::array end{
				CD3DX12_RESOURCE_BARRIER::Transition(slot->samples.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(depthResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)
			};
			command->ResourceBarrier(UINT(end.size()), end.data());
			Eval::Check(command->Close());
			Eval::Check(backend.commandQueue->Wait(slot->material->producerFence.get(), slot->material->producerValue));
			ID3D12CommandList* lists[]{ command };
			backend.commandQueue->ExecuteCommandLists(1, lists);
			submitted = true;
			const uint64_t value = nextFence + 1;
			Eval::Check(backend.commandQueue->Signal(completion.get(), value));
			nextFence = value; slot->readyValue = value;
			current = slot;
			dispatchedLights = lightCount;
			failure.clear();
		} catch (const std::exception& error) {
			failure = error.what();
			if (!selected || !*selected) return;
			if (submitted) {
				const uint64_t value = nextFence + 1;
				if (SUCCEEDED(backend.commandQueue->Signal(completion.get(), value))) {
					nextFence = value; (*selected)->readyValue = value;
				}
			} else {
				selected->reset();
			}
		}
	}
}

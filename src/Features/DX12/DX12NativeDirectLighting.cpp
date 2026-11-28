#include "DX12NativeDirectLighting.h"
#include "DX12GpuRetirement.h"
#include "GpuPass.h"
#include "Utils/FileSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <directx/d3dx12.h>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace DX12Native
{
	namespace
	{
		using Eval = NativeMaterialEvaluation;
		constexpr uint32_t kLightingStride = MaterialFrameContract::kLightingStride;
		constexpr uint64_t kCounterBytes = 8 * sizeof(uint32_t);
		constexpr size_t kLightRecordVectors = 7;
		constexpr uint64_t kLightUploadBytes = uint64_t(DX12::kMaxPreparedLights) * kLightRecordVectors * sizeof(std::array<float, 4>);
		struct Constants
		{
			Matrix projectionInverse, viewInverse;
			float4 positionAdjust;
			uint32_t width, height, materialPitch, lightCount;
			float nearLightRadius;
			uint32_t nearLightExclusion, instanceCount, parallaxShadows = 0;
			float4 directionalDirection, directionalRadiance;
			uint32_t foliageScatteringEnabled, characterCastersEnabled, quality, frame;
			uint32_t staticVisibilityEnabled;
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
		for (auto& slot : slots) {
			if (!slot || GpuFenceRetired(slot->readyFence.get(), slot->readyValue))
				continue;
			std::vector<FenceDependency> dependencies{ FencePoint(slot->readyFence.get(), slot->readyValue) };
			RetireAfterFences(std::move(slot), owner.d3d12Device, std::move(dependencies));
		}
	}

	void DirectLightingTrace::Poll(uint32_t frame)
	{
		if (current && current->frame != frame) current.reset();
		for (auto& slot : slots) {
			if (!slot) continue;
			if (slot->readyValue == UINT64_MAX && slot->readyFence && owner.commandQueue &&
				slot->readyFence->GetCompletedValue() != UINT64_MAX) {
				const uint64_t rescue = nextFence + 1;
				if (SUCCEEDED(owner.commandQueue->Signal(slot->readyFence.get(), rescue))) {
					nextFence = rescue;
					slot->readyValue = rescue;
				}
			}
			if (!GpuFenceRetired(slot->readyFence.get(), slot->readyValue)) continue;
			if (slot->statsPending) {
				slot->statsPending = false;
				if(slot->timingSample && slot->readyFence->GetCompletedValue()!=UINT64_MAX && (stats.gpuMilliseconds<0.0 || int32_t(slot->frame-stats.timingFrame)>=0)) {
					stats.gpuMilliseconds=slot->timing.Read(slot->readback.get(),kCounterBytes); stats.timingFrame=slot->frame;
				}
				if (slot->readyFence->GetCompletedValue() != UINT64_MAX &&
					(stats.frame == 0 || int32_t(slot->frame - stats.frame) >= 0)) {
					void* mapped = nullptr;
					const D3D12_RANGE read{ 0, SIZE_T(kCounterBytes) };
					if (SUCCEEDED(slot->readback->Map(0, &read, &mapped))) {
						stats.frame = slot->frame;
						std::memcpy(stats.counts.data(), mapped, kCounterBytes);
						const D3D12_RANGE noWrite{};
						slot->readback->Unmap(0, &noWrite);
					}
				}
			}
			if (slot.use_count() == 1) {
				slot->material.reset();
				slot->characters.reset();
			}
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
		const auto shader = Util::FileHelpers::ReadBoundedBinary(Util::PathHelpers::GetShadersPath() / "NativeComposite" / "DirectLightingV2.dxil", 16 * VideoMemoryBudget::kMiB);
		CD3DX12_DESCRIPTOR_RANGE ranges[4];
		ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, NativePBR::kGpuTextureDescriptorCapacity, 7, 0, 0);
		ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0, NativePBR::kGpuTextureDescriptorCapacity);
		ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 520);
		CD3DX12_ROOT_PARAMETER parameters[16];
		parameters[0].InitAsDescriptorTable(1, &ranges[2]);
		parameters[1].InitAsShaderResourceView(1);
		parameters[2].InitAsShaderResourceView(2);
		parameters[3].InitAsDescriptorTable(2, ranges);
		parameters[4].InitAsUnorderedAccessView(0);
		parameters[5].InitAsConstantBufferView(0);
		parameters[6].InitAsShaderResourceView(4);
		parameters[7].InitAsShaderResourceView(5);
		parameters[8].InitAsShaderResourceView(6);
		parameters[9].InitAsUnorderedAccessView(1);
		parameters[10].InitAsShaderResourceView(519);
		parameters[11].InitAsDescriptorTable(1, &ranges[3]);
		parameters[12].InitAsUnorderedAccessView(2);
		parameters[13].InitAsShaderResourceView(521);
		parameters[14].InitAsShaderResourceView(522);
		parameters[15].InitAsConstants(2,1);
		std::array<CD3DX12_STATIC_SAMPLER_DESC, 4> samplers;
		for (uint32_t mode = 0; mode < 4; ++mode)
			samplers[mode] = CD3DX12_STATIC_SAMPLER_DESC(mode, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
				(mode & 2) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
				(mode & 1) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
				D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
		const CD3DX12_ROOT_SIGNATURE_DESC signature(16, parameters, UINT(samplers.size()), samplers.data());
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
		std::span<const DirectPointLight> lights, bool inputsAvailable, const DirectDirectionalLight& directional,
		uint32_t viewportWidth, uint32_t viewportHeight, bool foliageScatteringEnabled, std::shared_ptr<const CharacterCasterFrame> characters, uint32_t quality)
	{
		CS_GPU_PASS("NativeLighting::Direct");
		if (!material || (current && current->material == material)) return;
		Poll(material->frame);
		dispatchedLights = 0;
		if (!inputsAvailable) { failure = "Waiting for native light inputs"; return; }
		if (!material->IsCoherent(material->frame, viewportWidth, viewportHeight) ||
			!material->descriptors ||
			!material->raster->depth || !material->raster->depth->resource || !material->raster->cameraValid) {
			failure = "Incompatible native material frame";
			return;
		}
		const uint32_t width = material->width, height = material->height;
		const uint64_t bytes = uint64_t(width) * height * kLightingStride;
		const uint64_t visibilityBytes = uint64_t(width) * height * sizeof(uint32_t);
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
			if (!selected) { failure = "Direct lighting frame pool busy"; return; }
			auto& slot = *selected;
			auto* device = backend.d3d12Device.get();
			const bool fresh = !slot || slot->width != width || slot->height != height;
			if (fresh) {
				const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				const uint64_t allocation = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
				const auto visibilityDesc = CD3DX12_RESOURCE_DESC::Buffer(visibilityBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				const uint64_t visibilityAllocation = device->GetResourceAllocationInfo(0, 1, &visibilityDesc).SizeInBytes;
				if (allocation == UINT64_MAX || visibilityAllocation == UINT64_MAX || visibilityAllocation > UINT64_MAX - 1024 * 1024 || allocation > UINT64_MAX - visibilityAllocation - 1024 * 1024)
					throw std::runtime_error("Direct lighting allocation unsupported");
				const uint64_t required = allocation + visibilityAllocation + 1024 * 1024;
				uint64_t tracked = 0;
				for (const auto& held : slots) if (held) tracked += held->allocationBytes;
				const auto budget = videoBudget.Query(device, tracked);
				if (!budget.localBudget || required > budget.effectiveBytes || tracked > budget.effectiveBytes - required)
					throw std::runtime_error("Direct lighting deferred by DXGI budget");
				auto replacement = std::make_shared<DirectLightingFrame>();
				replacement->allocationBytes = required;
				replacement->width = width; replacement->height = height; replacement->rowPitch = width * kLightingStride;
				replacement->samples = Eval::Buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				replacement->samples->SetName(L"NativeLighting::DiffuseSpecular");
				replacement->visibilityRowPitch = width * sizeof(uint32_t);
				replacement->visibility = Eval::Buffer(device, visibilityBytes, D3D12_HEAP_TYPE_DEFAULT,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				replacement->visibility->SetName(L"NativeLighting::VisibilityEvidence");
				D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
				heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				heapDesc.NumDescriptors = NativePBR::kGpuTextureDescriptorCapacity + 3;
				heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				Eval::Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(replacement->descriptors.put())));
				replacement->camera = Eval::Buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
				replacement->lights = Eval::Buffer(device, kLightUploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
				replacement->counters = Eval::Buffer(device, kCounterBytes, D3D12_HEAP_TYPE_DEFAULT,
					D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				replacement->readback = Eval::Buffer(device, kCounterBytes+16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
				replacement->counterInit = Eval::Table(device, std::vector<uint32_t>(8, 0));
				Eval::Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(replacement->allocator.put())));
				Eval::Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, replacement->allocator.get(), pipeline.get(), IID_PPV_ARGS(replacement->commands.put())));
				slot = std::move(replacement);
			} else {
				Eval::Check(slot->allocator->Reset());
				Eval::Check(slot->commands->Reset(slot->allocator.get(), pipeline.get()));
			}
			slot->readyFence = completion; slot->readyValue = UINT64_MAX; slot->statsPending = false;
			slot->root = root; slot->pipeline = pipeline;
			slot->frame = material->frame; slot->quality = SanitizeRTQuality(quality); slot->material = std::move(material);
			slot->timingSample=slot->frame%120u==0u && (slot->timing.heap || slot->timing.Initialize(device,backend.commandQueue.get()));
			slot->characters = characters && characters->Submitted(slot->frame, slot->material->scope) ? std::move(characters) : nullptr;
			slot->staticVisibilityAvailable = slot->material->HasSecondaryGeometry();
			slot->dynamicVisibilityAvailable = bool(slot->characters);
			const auto& raster = *slot->material->raster;
			auto& records = slot->lightScratch;
			records.clear();
			uint32_t shadowCount = 0, rejectedCount = 0;
			records.reserve(std::min(lights.size(), size_t(DX12::kMaxPreparedLights)) * kLightRecordVectors);
			for (const auto& light : lights) {
				if (records.size() / kLightRecordVectors >= DX12::kMaxPreparedLights) { ++rejectedCount; continue; }
				if (!std::isfinite(light.radius) || light.radius <= 0 ||
					!std::isfinite(light.position.x) || !std::isfinite(light.position.y) || !std::isfinite(light.position.z) ||
					!std::isfinite(light.radiance.x) || !std::isfinite(light.radiance.y) || !std::isfinite(light.radiance.z) ||
					!std::isfinite(light.sourceRadius) || !std::isfinite(light.fadeZone) ||
					(light.spot && !std::ranges::all_of(light.worldToSpot, [](float v) { return std::isfinite(v); }))) { ++rejectedCount; continue; }
				shadowCount += light.castsShadow ? 1u : 0u;
				records.push_back({ light.position.x, light.position.y, light.position.z, light.radius });
				// Preserve the seven-vector ABI: color.w == 1 explicitly opts out of shadows.
				records.push_back({ light.radiance.x, light.radiance.y, light.radiance.z, light.castsShadow ? 0.0f : 1.0f });
				records.push_back({ light.sourceRadius, light.fadeZone, light.inverseSquare ? 1.0f : 0.0f, light.spot ? 1.0f : 0.0f });
				for (size_t row = 0; row < 4; ++row) {
					const size_t offset = row * 4;
					records.push_back({ light.worldToSpot[offset], light.worldToSpot[offset + 1], light.worldToSpot[offset + 2], light.worldToSpot[offset + 3] });
				}
			}
			const uint32_t lightCount = uint32_t(records.size() / kLightRecordVectors);
			// Invalid individual emitters do not invalidate otherwise usable receiver tiles.
			// SelectedDirectLights remains diagnostic; a zero-light result is not full coverage.
			if (records.empty()) records.resize(kLightRecordVectors);
			slot->lightUploadUpdated = records != slot->packedLights;
			if (slot->lightUploadUpdated) {
				void* lightUpload = nullptr;
				const D3D12_RANGE noRead{};
				Eval::Check(slot->lights->Map(0, &noRead, &lightUpload));
				const auto writtenBytes = records.size() * sizeof(records.front());
				std::memcpy(lightUpload, records.data(), writtenBytes);
				const D3D12_RANGE written{0, writtenBytes};
				slot->lights->Unmap(0, &written);
				slot->packedLights.swap(records);
			}
			slot->shadowLightCount = shadowCount;
			slot->rejectedLightCount = rejectedCount;
			const Constants constants{ raster.camera.CameraProjInverse, raster.camera.CameraViewInverse,
				raster.camera.CameraPosAdjust, width, height, slot->material->rowPitch, lightCount,
				0.0f, 0u, slot->staticVisibilityAvailable ? slot->material->scene->materialEvaluation->instanceCount : 0u, slot->material->parallaxShadows ? 1u : 0u, directional.direction, directional.radiance, foliageScatteringEnabled ? 1u : 0u, slot->characters ? 1u : 0u, slot->quality, slot->frame, slot->staticVisibilityAvailable ? 1u : 0u };
			slot->directionalIncluded = directional.direction.w != 0.0f;
			slot->directional = directional;
			slot->lightCount = lightCount;
			slot->foliageScattering = foliageScatteringEnabled;
			WriteConstants(slot->camera.get(), constants);
			auto* depthResource = raster.depth->resource.get();
			const auto* scene = slot->staticVisibilityAvailable ? slot->material->scene.get() : nullptr;
			const auto cpu = slot->descriptors->GetCPUDescriptorHandleForHeapStart();
			const auto gpu = slot->descriptors->GetGPUDescriptorHandleForHeapStart();
			const auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			const auto cap = NativePBR::kGpuTextureDescriptorCapacity;
			// Shader-visible heaps cannot be descriptor-copy sources. Recreate views
			// from the immutable material evaluation, retaining the same resources.
			slot->material->rasterMaterialEvaluation->WriteTextureViews(device, cpu);
			D3D12_SHADER_RESOURCE_VIEW_DESC depthView{};
			depthView.Format = DXGI_FORMAT_R32_FLOAT;
			depthView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			depthView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			depthView.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(depthResource, &depthView, {cpu.ptr + SIZE_T(cap) * step});
			D3D12_SHADER_RESOURCE_VIEW_DESC as{};
			as.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
			as.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			as.RaytracingAccelerationStructure.Location = scene ? scene->acceleration->GetGPUVirtualAddress() : 0;
			device->CreateShaderResourceView(nullptr, &as, {cpu.ptr + SIZE_T(cap + 1) * step});
			as.RaytracingAccelerationStructure.Location = slot->characters ? slot->characters->acceleration->GetGPUVirtualAddress() : 0;
			device->CreateShaderResourceView(nullptr, &as, {cpu.ptr + SIZE_T(cap + 2) * step});
			auto* command = slot->commands.get();
			ID3D12DescriptorHeap* heaps[]{ slot->descriptors.get() };
			command->SetDescriptorHeaps(1, heaps);
			command->SetComputeRootSignature(root.get());
			command->SetComputeRootDescriptorTable(0, {gpu.ptr + UINT64(cap + 1) * step});
			command->SetComputeRootShaderResourceView(1, slot->material->samples->GetGPUVirtualAddress());
			command->SetComputeRootShaderResourceView(2, slot->lights->GetGPUVirtualAddress());
			command->SetComputeRootDescriptorTable(3, gpu);
			command->SetComputeRootUnorderedAccessView(4, slot->samples->GetGPUVirtualAddress());
			command->SetComputeRootConstantBufferView(5, slot->camera->GetGPUVirtualAddress());
			const auto unusedAddress = slot->material->samples->GetGPUVirtualAddress();
			command->SetComputeRootShaderResourceView(6, scene ? scene->shadingGeometry->buffer->GetGPUVirtualAddress() : unusedAddress);
			command->SetComputeRootShaderResourceView(7, scene ? scene->materialEvaluation->records->GetGPUVirtualAddress() : unusedAddress);
			command->SetComputeRootShaderResourceView(8, scene ? scene->materialEvaluation->bindings->GetGPUVirtualAddress() : unusedAddress);
			command->SetComputeRootUnorderedAccessView(9, slot->counters->GetGPUVirtualAddress());
			command->SetComputeRootShaderResourceView(10, slot->material->materialLobes->GetGPUVirtualAddress());
			command->SetComputeRootDescriptorTable(11, {gpu.ptr + UINT64(cap + 2) * step});
			command->SetComputeRootUnorderedAccessView(12, slot->visibility->GetGPUVirtualAddress());
			const auto specialized=slot->material->HasSpecialized();
			command->SetComputeRootShaderResourceView(13, slot->material->specializedDirectory->GetGPUVirtualAddress());
			command->SetComputeRootShaderResourceView(14, slot->material->specializedPool->GetGPUVirtualAddress());
			const uint32_t specializedConstants[]{slot->material->specializedCapacity,specialized?1u:0u};
			command->SetComputeRoot32BitConstants(15,2,specializedConstants,0);
			if (!fresh) {
				const auto counterStart = CD3DX12_RESOURCE_BARRIER::Transition(slot->counters.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
				command->ResourceBarrier(1, &counterStart);
			}
			command->CopyBufferRegion(slot->counters.get(), 0, slot->counterInit.get(), 0, kCounterBytes);
			const auto counterUav = CD3DX12_RESOURCE_BARRIER::Transition(slot->counters.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			command->ResourceBarrier(1, &counterUav);
			const auto depthStart = CD3DX12_RESOURCE_BARRIER::Transition(depthResource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			command->ResourceBarrier(1, &depthStart);
			if (!fresh) {
				const auto outputStart = CD3DX12_RESOURCE_BARRIER::Transition(slot->samples.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				command->ResourceBarrier(1, &outputStart);
				const auto visibilityStart = CD3DX12_RESOURCE_BARRIER::Transition(slot->visibility.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				command->ResourceBarrier(1, &visibilityStart);
			}
			if(slot->timingSample) slot->timing.Begin(command);
			command->Dispatch((ShadowPhysicalWidth(ShadowLogicalExtent(width, slot->quality), slot->quality) + 7) / 8,
				(ShadowLogicalExtent(height, slot->quality) + 7) / 8, 1);
			if(slot->timingSample) slot->timing.End(command,slot->readback.get(),kCounterBytes);
			const std::array end{
				CD3DX12_RESOURCE_BARRIER::Transition(slot->samples.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(slot->visibility.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(depthResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)
			};
			command->ResourceBarrier(UINT(end.size()), end.data());
			const auto counterEnd = CD3DX12_RESOURCE_BARRIER::Transition(slot->counters.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
			command->ResourceBarrier(1, &counterEnd);
			command->CopyBufferRegion(slot->readback.get(), 0, slot->counters.get(), 0, kCounterBytes);
			Eval::Check(command->Close());
			Eval::Check(backend.commandQueue->Wait(slot->material->producerFence.get(), slot->material->producerValue));
			if (slot->characters) Eval::Check(backend.commandQueue->Wait(slot->characters->readyFence.get(), slot->characters->readyValue));
			ID3D12CommandList* lists[]{ command };
			backend.commandQueue->ExecuteCommandLists(1, lists);
			submitted = true;
			const uint64_t value = nextFence + 1;
			Eval::Check(backend.commandQueue->Signal(completion.get(), value));
			nextFence = value; slot->readyValue = value; slot->statsPending = true;
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

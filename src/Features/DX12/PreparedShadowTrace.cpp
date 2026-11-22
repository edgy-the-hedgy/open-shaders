#include "PreparedShadowTrace.h"
#include "Features/Upscaling.h"
#include "GpuPass.h"
#include "Utils/D3D.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <directx/d3dx12.h>
#include <fstream>
#include <ranges>

namespace DX12
{
	namespace
	{
		struct GpuLight
		{
			float4 positionRadius, colorFade, emitterFalloff;
		};
		struct alignas(16) MaskCamera
		{
			Matrix projectionInverse, viewInverse;
			float4 adjust;
			uint32_t width, height, count;
			float strength = 1.0f;
			uint32_t samplingMode = 0, samplingPhase = 0, spatialReconstruction = 1, batchIndex = 0;
		};
		static_assert(sizeof(MaskCamera) == 176);
		static_assert(sizeof(GpuLight) == 48);
		constexpr uint32_t kBatchSize = 64;
		constexpr size_t kCameraStride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

		bool Upload(ID3D12Device* device, winrt::com_ptr<ID3D12Resource>& target,
			const void* data, size_t size)
		{
			if (!size || !data) return false;
			if (!target || target->GetDesc().Width < size) {
				target = nullptr;
				const auto desc = CD3DX12_RESOURCE_DESC::Buffer((size + 255u) & ~size_t(255u));
				const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
				if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(target.put()))))
					return false;
			}
			void* mapped = nullptr;
			if (FAILED(target->Map(0, nullptr, &mapped)) || !mapped) return false;
			std::memcpy(mapped, data, size);
			target->Unmap(0, nullptr);
			return true;
		}
	}

	PreparedShadowTrace::~PreparedShadowTrace()
	{
		current.reset();
		for (auto& slot : slots) {
			if (!slot) continue;
			const auto done = completion ? completion->GetCompletedValue() : 0;
			if (done == UINT64_MAX || slot->readyValue > done ||
				std::ranges::any_of(slot->consumers, [](const auto& consumer) {
					const auto completed = consumer.first->GetCompletedValue();
					return completed == UINT64_MAX || completed < consumer.second;
				}))
				(void)new std::shared_ptr<RawShadowFrame>(std::move(slot));
		}
	}

	bool PreparedShadowTrace::Initialize(ID3D12Device* candidate)
	{
		if (root && pipeline && device.get() == candidate) return true;
		if (!candidate) return false;
		try {
			std::ifstream file("Data/Shaders/DX12NativeComposite/PreparedShadowMask.dxil",
				std::ios::binary | std::ios::ate);
			if (!file || file.tellg() <= 0) return false;
			const size_t size = size_t(file.tellg());
			if (size > 16u * 1024u * 1024u) return false;
			std::vector<char> shader(size);
			file.seekg(0);
			if (!file.read(shader.data(), std::streamsize(size))) return false;
			D3D12_DESCRIPTOR_RANGE depth{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0};
			D3D12_DESCRIPTOR_RANGE bits{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
			std::array<D3D12_ROOT_PARAMETER, 5> params{};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			params[0].Descriptor.ShaderRegister = 0;
			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[1].DescriptorTable = {1, &depth};
			params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			params[2].Descriptor.ShaderRegister = 2;
			params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[3].DescriptorTable = {1, &bits};
			params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[4].Descriptor.ShaderRegister = 0;
			D3D12_ROOT_SIGNATURE_DESC signature{};
			signature.NumParameters = UINT(params.size());
			signature.pParameters = params.data();
			winrt::com_ptr<ID3DBlob> serialized, errors;
			DX::ThrowIfFailed(D3D12SerializeRootSignature(&signature,
				D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
			winrt::com_ptr<ID3D12RootSignature> nextRoot;
			DX::ThrowIfFailed(candidate->CreateRootSignature(0, serialized->GetBufferPointer(),
				serialized->GetBufferSize(), IID_PPV_ARGS(nextRoot.put())));
			D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
			desc.pRootSignature = nextRoot.get();
			desc.CS = {shader.data(), shader.size()};
			winrt::com_ptr<ID3D12PipelineState> nextPipeline;
			DX::ThrowIfFailed(candidate->CreateComputePipelineState(&desc, IID_PPV_ARGS(nextPipeline.put())));
			winrt::com_ptr<ID3D12Fence> fence;
			DX::ThrowIfFailed(candidate->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())));
			device.copy_from(candidate);
			root = std::move(nextRoot);
			pipeline = std::move(nextPipeline);
			completion = std::move(fence);
			status.clear();
			return true;
		} catch (const std::exception& e) {
			status = e.what();
			return false;
		}
	}

	void PreparedShadowTrace::Dispatch(std::shared_ptr<const RasterFrame> raster,
		std::shared_ptr<const AccelerationSceneReady> scene,
		std::shared_ptr<const ResolvedLights> lights, uint32_t sampling, uint32_t phase)
	{
		CS_GPU_PASS("DX12NativeScene::TracePreparedShadowMask");
		current.reset();
		auto& backend = globals::features::upscaling.dx12SwapChain;
		if (!raster || !raster->depth || !scene || !scene->acceleration || !lights ||
			!raster->readyFence || !raster->readyValue || !Initialize(backend.d3d12Device.get()))
			return;
		std::vector<GpuLight> source;
		source.reserve(lights->values.size());
		for (const auto& entry : lights->values) {
			const auto& l = entry.light;
			if (!std::isfinite(l.radius) || l.radius <= 0 ||
				!std::isfinite(l.position.x) || !std::isfinite(l.position.y) ||
				!std::isfinite(l.position.z)) continue;
			source.push_back({{l.position.x, l.position.y, l.position.z, l.radius},
				{l.color.x, l.color.y, l.color.z, l.fade},
				{l.emitterRadius, l.falloffExponent, 0, 0}});
		}
		const uint32_t count = uint32_t(source.size());
		if (!count) return;
		const uint32_t batches = (count + kBatchSize - 1) / kBatchSize;
		if (batches > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) return;
		const auto done = completion->GetCompletedValue();
		const auto available = std::find_if(slots.begin(), slots.end(), [&](const auto& slot) {
			if (!slot) return true;
			if (slot.use_count() != 1 || done == UINT64_MAX) return false;
			if (!slot->readyValue) return true; // Reuse a slot that never reached submission.
			if (done < slot->readyValue) return false;
			return std::ranges::all_of(slot->consumers, [](const auto& consumer) {
				const auto value = consumer.first->GetCompletedValue();
				return value != UINT64_MAX && value >= consumer.second;
			});
		});
		if (available == slots.end()) return;
		try {
			auto& slot = *available;
			if (!slot || slot->width != raster->width ||
				slot->height != raster->height || slot->batchCount != batches) {
				slot.reset();
				slot = std::make_shared<RawShadowFrame>();
				slot->width = raster->width;
				slot->height = raster->height;
				slot->batchCount = batches;
				const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
				const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_UINT,
					slot->width, slot->height, UINT16(batches), 1, 1, 0,
					D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				DX::ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(slot->bits.put())));
				D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
				heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				heapDesc.NumDescriptors = 2;
				heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				DX::ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(slot->descriptors.put())));
				DX::ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
					IID_PPV_ARGS(slot->allocator.put())));
				DX::ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
					slot->allocator.get(), pipeline.get(), IID_PPV_ARGS(slot->commands.put())));
			} else {
				DX::ThrowIfFailed(slot->allocator->Reset());
				DX::ThrowIfFailed(slot->commands->Reset(slot->allocator.get(), pipeline.get()));
			}
			slot->raster = std::move(raster);
			slot->scene = std::move(scene);
			slot->preparedLights = std::move(lights);
			slot->frame = slot->raster->frame;
			slot->lightCount = count;
			slot->samplingMode = sampling;
			slot->samplingPhase = phase;
			slot->consumers.clear();
			const auto& camera = slot->raster->camera;
			MaskCamera constants{camera.CameraProjInverse, camera.CameraViewInverse,
				camera.CameraPosAdjust, slot->width, slot->height, count, 1.0f,
				sampling, phase, 1u, 0u};
			std::vector<std::array<uint8_t, kCameraStride>> cameraRecords(batches);
			for (uint32_t i = 0; i < batches; ++i) {
				constants.batchIndex = i;
				std::memcpy(cameraRecords[i].data(), &constants, sizeof(constants));
			}
			if (!Upload(device.get(), slot->camera, cameraRecords.data(),
				cameraRecords.size() * kCameraStride) ||
				!Upload(device.get(), slot->lights, source.data(), source.size() * sizeof(GpuLight)))
				return;
			const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			auto cpu = slot->descriptors->GetCPUDescriptorHandleForHeapStart();
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Texture2D.MipLevels = 1;
			srv.Format = DXGI_FORMAT_R32_FLOAT;
			device->CreateShaderResourceView(slot->raster->depth->resource.get(), &srv, cpu);
			cpu.ptr += step;
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
			uav.Texture2DArray.ArraySize = batches;
			uav.Format = DXGI_FORMAT_R32G32B32A32_UINT;
			device->CreateUnorderedAccessView(slot->bits.get(), nullptr, &uav, cpu);
			ID3D12DescriptorHeap* heaps[]{slot->descriptors.get()};
			auto* cmd = slot->commands.get();
			cmd->SetDescriptorHeaps(1, heaps);
			std::array<D3D12_RESOURCE_BARRIER, 2> begin{
				CD3DX12_RESOURCE_BARRIER::Transition(slot->raster->depth->resource.get(),
					D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(slot->bits.get(),
					D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
			cmd->ResourceBarrier(UINT(begin.size()), begin.data());
			cmd->SetComputeRootSignature(root.get());
			cmd->SetPipelineState(pipeline.get());
			cmd->SetComputeRootShaderResourceView(0, slot->scene->acceleration->GetGPUVirtualAddress());
			auto gpu = slot->descriptors->GetGPUDescriptorHandleForHeapStart();
			cmd->SetComputeRootDescriptorTable(1, gpu);
			cmd->SetComputeRootShaderResourceView(2, slot->lights->GetGPUVirtualAddress());
			gpu.ptr += step;
			cmd->SetComputeRootDescriptorTable(3, gpu);
			for (uint32_t batch = 0; batch < batches; ++batch) {
				cmd->SetComputeRootConstantBufferView(4,
					slot->camera->GetGPUVirtualAddress() + uint64_t(batch) * kCameraStride);
				cmd->Dispatch((slot->width + 7) / 8, (slot->height + 7) / 8, 1);
			}
			std::array<D3D12_RESOURCE_BARRIER, 2> end{
				CD3DX12_RESOURCE_BARRIER::Transition(slot->raster->depth->resource.get(),
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(slot->bits.get(),
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)};
			cmd->ResourceBarrier(UINT(end.size()), end.data());
			DX::ThrowIfFailed(cmd->Close());
			DX::ThrowIfFailed(backend.commandQueue->Wait(slot->raster->readyFence.get(),
				slot->raster->readyValue));
			ID3D12CommandList* lists[]{cmd};
			backend.commandQueue->ExecuteCommandLists(1, lists);
			const uint64_t ready = ++nextFence;
			DX::ThrowIfFailed(backend.commandQueue->Signal(completion.get(), ready));
			slot->readyFence = completion;
			slot->readyValue = ready;
			current = slot;
			status.clear();
		} catch (const std::exception& e) {
			status = e.what();
		}
	}
	std::shared_ptr<const RawShadowFrame> PreparedShadowTrace::Get(uint32_t frame) const
	{
		return current && current->frame == frame ? current : nullptr;
	}
}

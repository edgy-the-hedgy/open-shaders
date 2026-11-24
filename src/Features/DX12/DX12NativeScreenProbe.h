#pragma once

#include "DX12NativeAcceleration.h"
#include "DX12NativeRasterCapture.h"
#include "DX12GpuRetirement.h"
#include <fstream>

namespace DX12Native
{
	// Optional low-density visible-pixel diagnostic. No final-color writes,
	// no full-resolution targets, and no CPU fence waits.
	class NativeScreenProbe
	{
		using Eval = NativeMaterialEvaluation;
		static constexpr uint32_t kWidth = 64, kHeight = 36, kCount = kWidth * kHeight;
		struct Camera
		{
			Matrix projectionInverse, viewInverse;
			float4 positionAdjust;
			uint32_t width, height, gridWidth, gridHeight;
		};
		static_assert(sizeof(Camera) <= 256);
		struct Submission
		{
			std::shared_ptr<const RasterFrame> raster;
			std::shared_ptr<const TopLevel> scene;
			winrt::com_ptr<ID3D12Resource> output, readback, camera;
			winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
			winrt::com_ptr<ID3D12RootSignature> root;
			winrt::com_ptr<ID3D12PipelineState> pipeline;
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> commands;
			winrt::com_ptr<ID3D12Fence> fence;
			uint64_t value = UINT64_MAX;
			uint32_t frame = 0;
			bool discard = false;
		};
		std::shared_ptr<Submission> pending;
		GraphicsBackend owner;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12Fence> fence;
		uint64_t nextValue = 0;
		uint32_t lastFrame = UINT32_MAX;

		void Initialize(const GraphicsBackend& backend)
		{
			if (pipeline) {
				if (owner.d3d12Device.get() != backend.d3d12Device.get() ||
					owner.commandQueue.get() != backend.commandQueue.get())
					throw std::runtime_error("Screen probe device changed; restart required");
				return;
			}
			auto* device = backend.d3d12Device.get();
			std::ifstream file("Data/Shaders/NativeComposite/NativePBRScreenProbe.dxil", std::ios::binary | std::ios::ate);
			if (!file || file.tellg() <= 0 || file.tellg() > 16*1024*1024)
				throw std::runtime_error("NativePBRScreenProbe.dxil unavailable");
			std::vector<char> shader(static_cast<size_t>(file.tellg()));
			file.seekg(0);
			if (!file.read(shader.data(), std::streamsize(shader.size())))
				throw std::runtime_error("Screen probe DXIL read failed");
			CD3DX12_DESCRIPTOR_RANGE range;
			range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, NativePBR::kGpuTextureDescriptorCapacity + 1, 3);
			CD3DX12_ROOT_PARAMETER parameters[8];
			parameters[0].InitAsShaderResourceView(0);
			parameters[1].InitAsShaderResourceView(1);
			parameters[2].InitAsShaderResourceView(2);
			parameters[3].InitAsDescriptorTable(1, &range);
			parameters[4].InitAsUnorderedAccessView(0);
			parameters[5].InitAsConstants(1, 0);
			parameters[6].InitAsShaderResourceView(516);
			parameters[7].InitAsConstantBufferView(1);
			std::array<CD3DX12_STATIC_SAMPLER_DESC, 4> samplers;
			for (uint32_t mode = 0; mode < 4; ++mode)
				samplers[mode] = CD3DX12_STATIC_SAMPLER_DESC(mode, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
					(mode & 2) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
					(mode & 1) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
					D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
			const CD3DX12_ROOT_SIGNATURE_DESC desc(8, parameters, UINT(samplers.size()), samplers.data());
			winrt::com_ptr<ID3DBlob> blob, errors;
			winrt::com_ptr<ID3D12RootSignature> newRoot;
			winrt::com_ptr<ID3D12PipelineState> newPipeline;
			winrt::com_ptr<ID3D12Fence> newFence;
			Eval::Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,blob.put(),errors.put()));
			Eval::Check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(newRoot.put())));
			D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
			pso.pRootSignature=newRoot.get(); pso.CS={shader.data(),shader.size()};
			Eval::Check(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(newPipeline.put())));
			Eval::Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(newFence.put())));
			owner=backend; root=std::move(newRoot); pipeline=std::move(newPipeline); fence=std::move(newFence);
		}
	public:
		struct Stats
		{
			uint32_t frame=0, sampled=0, evaluated=0, noHit=0, depthMismatch=0, missingMaterial=0, invalid=0;
		};
		Stats stats{};
		std::string failure;

		~NativeScreenProbe()
		{
			if (pending && !GpuFenceRetired(pending->fence.get(),pending->value))
				RetireWhenGpuDone(std::move(pending),owner.d3d12Device,[](const Submission& submission) {
					return GpuFenceRetired(submission.fence.get(),submission.value);
				});
		}
		void Reset()
		{
			stats={}; failure.clear(); lastFrame=UINT32_MAX;
			if (pending) pending->discard=true;
		}
		void Poll()
		{
			if (!pending) return;
			const auto done=pending->fence->GetCompletedValue();
			if (done==UINT64_MAX) { pending.reset(); failure="Screen probe device removed"; return; }
			if (pending->value==UINT64_MAX || done<pending->value) return;
			if (!pending->discard) {
				Stats result{};
				result.frame=pending->frame; result.sampled=kCount;
				const D3D12_RANGE read{0,SIZE_T(kCount)*sizeof(Eval::Sample)};
				void* mapped=nullptr;
				if (SUCCEEDED(pending->readback->Map(0,&read,&mapped))) {
					const auto* samples=static_cast<const Eval::Sample*>(mapped);
					for (uint32_t i=0;i<kCount;++i) {
						const float status=samples[i].color[3];
						if (status==1.0f) ++result.evaluated;
						else if (status==-1.0f) ++result.noHit;
						else if (status==-2.0f) ++result.depthMismatch;
						else if (status==-3.0f) ++result.missingMaterial;
						else ++result.invalid;
					}
					const D3D12_RANGE noWrite{};
					pending->readback->Unmap(0,&noWrite);
					stats=result; failure.clear();
				}
			}
			pending.reset();
		}
		bool WantsFrame(uint32_t frame) const
		{
			return !pending && (lastFrame==UINT32_MAX || frame-lastFrame>=30u);
		}
		void Dispatch(const GraphicsBackend& backend,std::shared_ptr<const RasterFrame> raster,
			std::shared_ptr<const TopLevel> scene,uint32_t frame,uint32_t width,uint32_t height)
		{
			if (!WantsFrame(frame)) return;
			lastFrame=frame;
			if (!raster || !raster->depth || !raster->depth->resource || !raster->cameraValid || !raster->readyFence ||
				!scene || !scene->acceleration || !scene->producerFence || !scene->shadingGeometry || !scene->materialEvaluation ||
				!width || !height || width>raster->width || height>raster->height) {
				failure="Waiting for matching raster and native material generation"; return;
			}
			const auto& evaluation=*scene->materialEvaluation;
			if (evaluation.revision!=scene->shadingGeometry->revision || evaluation.revision!=scene->geometryRevision ||
				evaluation.instanceCount!=scene->shadingGeometry->instanceCount || !evaluation.residentRecipes) {
				failure="No coherent resident material generation"; return;
			}
			bool submitted=false;
			try {
				Initialize(backend);
				auto* device=backend.d3d12Device.get();
				VideoMemoryBudget budget;
				const auto available=budget.Query(device,0);
				constexpr uint64_t bytes=uint64_t(kCount)*sizeof(Eval::Sample);
				if (!available.dxgiAvailable || available.effectiveBytes<2*bytes+4ull*65536) {
					failure="Screen probe deferred by DXGI budget"; return;
				}
				auto slot=std::make_shared<Submission>();
				slot->raster=std::move(raster); slot->scene=std::move(scene);
				slot->frame=frame; slot->fence=fence; slot->root=root; slot->pipeline=pipeline;
				slot->output=Eval::Buffer(device,bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				slot->readback=Eval::Buffer(device,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
				slot->camera=Eval::Buffer(device,256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
				const Camera constants{slot->raster->camera.CameraProjInverse,slot->raster->camera.CameraViewInverse,
					slot->raster->camera.CameraPosAdjust,width,height,kWidth,kHeight};
				void* mapped=nullptr; const D3D12_RANGE noRead{};
				Eval::Check(slot->camera->Map(0,&noRead,&mapped));
				std::memcpy(mapped,&constants,sizeof(constants)); slot->camera->Unmap(0,nullptr);
				const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
					NativePBR::kGpuTextureDescriptorCapacity+1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
				Eval::Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(slot->descriptors.put())));
				const auto cpu=slot->descriptors->GetCPUDescriptorHandleForHeapStart();
				const auto step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				for (uint32_t i=0;i<NativePBR::kGpuTextureDescriptorCapacity;++i) {
					const auto pool=i%NativePBR::kGpuTexturePoolCapacity;
					const auto* texture=pool<evaluation.poolTextures.size()?evaluation.poolTextures[pool].get():nullptr;
					D3D12_SHADER_RESOURCE_VIEW_DESC view{};
					view.Format=texture?(i>=NativePBR::kGpuTexturePoolCapacity?DirectX::MakeSRGB(texture->format):texture->format):DXGI_FORMAT_R8G8B8A8_UNORM;
					view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
					view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					view.Texture2DArray.MipLevels=texture?texture->mips:1;
					view.Texture2DArray.ArraySize=1;
					device->CreateShaderResourceView(texture?texture->resource.get():nullptr,&view,{cpu.ptr+SIZE_T(i)*step});
				}
				D3D12_SHADER_RESOURCE_VIEW_DESC depth{};
				depth.Format=DXGI_FORMAT_R32_FLOAT; depth.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
				depth.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; depth.Texture2D.MipLevels=1;
				device->CreateShaderResourceView(slot->raster->depth->resource.get(),&depth,
					{cpu.ptr+SIZE_T(NativePBR::kGpuTextureDescriptorCapacity)*step});
				Eval::Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(slot->allocator.put())));
				Eval::Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot->allocator.get(),pipeline.get(),IID_PPV_ARGS(slot->commands.put())));
				auto* command=slot->commands.get();
				ID3D12DescriptorHeap* heaps[]{slot->descriptors.get()};
				command->SetDescriptorHeaps(1,heaps); command->SetComputeRootSignature(root.get());
				command->SetComputeRootShaderResourceView(0,slot->scene->shadingGeometry->buffer->GetGPUVirtualAddress());
				command->SetComputeRootShaderResourceView(1,evaluation.records->GetGPUVirtualAddress());
				command->SetComputeRootShaderResourceView(2,evaluation.bindings->GetGPUVirtualAddress());
				command->SetComputeRootDescriptorTable(3,slot->descriptors->GetGPUDescriptorHandleForHeapStart());
				command->SetComputeRootUnorderedAccessView(4,slot->output->GetGPUVirtualAddress());
				command->SetComputeRoot32BitConstant(5,evaluation.instanceCount,0);
				command->SetComputeRootShaderResourceView(6,slot->scene->acceleration->GetGPUVirtualAddress());
				command->SetComputeRootConstantBufferView(7,slot->camera->GetGPUVirtualAddress());
				const auto begin=CD3DX12_RESOURCE_BARRIER::Transition(slot->raster->depth->resource.get(),
					D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				command->ResourceBarrier(1,&begin);
				command->Dispatch((kWidth+7)/8,(kHeight+7)/8,1);
				const std::array end{
					CD3DX12_RESOURCE_BARRIER::Transition(slot->output.get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE),
					CD3DX12_RESOURCE_BARRIER::Transition(slot->raster->depth->resource.get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON)};
				command->ResourceBarrier(UINT(end.size()),end.data());
				command->CopyBufferRegion(slot->readback.get(),0,slot->output.get(),0,bytes);
				Eval::Check(command->Close());
				Eval::Check(backend.commandQueue->Wait(slot->raster->readyFence.get(),slot->raster->readyValue));
				Eval::Check(backend.commandQueue->Wait(slot->scene->producerFence.get(),slot->scene->producerValue));
				pending=std::move(slot);
				ID3D12CommandList* lists[]{command}; backend.commandQueue->ExecuteCommandLists(1,lists);
				submitted=true;
				const auto value=nextValue+1;
				Eval::Check(backend.commandQueue->Signal(fence.get(),value));
				nextValue=value; pending->value=value;
			} catch (const std::exception& error) {
				failure=error.what();
				if (submitted && pending) {
					pending->discard=true;
					const auto rescue=nextValue+1;
					if (SUCCEEDED(backend.commandQueue->Signal(fence.get(),rescue))) {
						nextValue=rescue; pending->value=rescue;
					}
				} else pending.reset();
			}
		}
	};
}

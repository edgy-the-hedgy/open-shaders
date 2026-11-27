#include "DX12FullNativeReconstruction.h"
#include "DX12GpuRetirement.h"
#include "Utils/FileSystem.h"

#include <directx/d3dx12.h>
#include <fstream>

namespace DX12Native
{
	using Eval = NativeMaterialEvaluation;
	using namespace MaterialFrameContract;

	FullNativeReconstruction::~FullNativeReconstruction()
	{
		current.reset();
		for (auto& slot : slots) {
			if (!slot || GpuFenceRetired(slot->readyFence.get(), slot->readyValue))
				continue;
			std::vector<FenceDependency> dependencies{ FencePoint(slot->readyFence.get(), slot->readyValue) };
			RetireAfterFences(std::move(slot), owner.d3d12Device, std::move(dependencies));
		}
	}

	void FullNativeReconstruction::Poll(uint32_t frame)
	{
		if (current && current->frame != frame)
			current.reset();
		for (auto& slot : slots) {
			if (!slot || slot.use_count() != 1)
				continue;
			if (slot->readyValue == UINT64_MAX && completion && owner.commandQueue &&
				completion->GetCompletedValue() != UINT64_MAX) {
				const auto rescue = nextValue + 1;
				if (SUCCEEDED(owner.commandQueue->Signal(completion.get(), rescue))) {
					nextValue = rescue;
					slot->readyValue = rescue;
				}
			}
			if (GpuFenceRetired(slot->readyFence.get(), slot->readyValue)) {
				slot->material.reset();
				slot->direct.reset();
				slot->indirect.reset();
			}
		}
	}

	std::shared_ptr<const FullNativeReconstructionFrame> FullNativeReconstruction::Get(uint32_t frame) const
	{
		return current && current->frame == frame && current->readyFence &&
			ValidProducerSignal(current->readyValue, current->readyFence->GetCompletedValue()) ? current : nullptr;
	}

	void FullNativeReconstruction::Initialize(const GraphicsBackend& backend)
	{
		if (pipeline) {
			if (owner.d3d12Device.get() != backend.d3d12Device.get() || owner.commandQueue.get() != backend.commandQueue.get())
				throw std::runtime_error("Full Native reconstruction backend changed");
			return;
		}
		auto* device = backend.d3d12Device.get();
		std::ifstream file(Util::PathHelpers::GetShadersPath() / "NativeComposite" / "FullNativeReconstruction.dxil", std::ios::binary | std::ios::ate);
		if (!file || file.tellg() <= 0 || file.tellg() > 16 * 1024 * 1024)
			throw std::runtime_error("Full Native reconstruction DXIL unavailable");
		std::vector<char> shader(static_cast<size_t>(file.tellg()));
		file.seekg(0);
		if (!file.read(shader.data(), std::streamsize(shader.size())))
			throw std::runtime_error("Full Native reconstruction DXIL read failed");
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);
		CD3DX12_ROOT_PARAMETER parameters[6];
		parameters[0].InitAsShaderResourceView(0);
		parameters[1].InitAsShaderResourceView(1);
		parameters[2].InitAsShaderResourceView(2);
		parameters[3].InitAsDescriptorTable(1, &range);
		parameters[4].InitAsUnorderedAccessView(0);
		parameters[5].InitAsConstants(6, 0);
		CD3DX12_ROOT_SIGNATURE_DESC signature(6, parameters);
		winrt::com_ptr<ID3DBlob> blob, errors;
		winrt::com_ptr<ID3D12RootSignature> newRoot;
		winrt::com_ptr<ID3D12PipelineState> newPipeline;
		winrt::com_ptr<ID3D12Fence> newFence;
		DX::ThrowIfFailed(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put()));
		DX::ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(newRoot.put())));
		D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
		pso.pRootSignature = newRoot.get();
		pso.CS = { shader.data(), shader.size() };
		DX::ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(newPipeline.put())));
		DX::ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(newFence.put())));
		owner = backend;
		root = std::move(newRoot);
		pipeline = std::move(newPipeline);
		completion = std::move(newFence);
	}

	void FullNativeReconstruction::Dispatch(const GraphicsBackend& backend,
		std::shared_ptr<const NativeScreenProbe::MaterialFrame> material,
		std::shared_ptr<const DirectLightingFrame> direct, std::shared_ptr<const IndirectLightingFrame> indirect)
	{
		if (!material || !material->IsCoherent(material->frame, material->width, material->height))
			return;
		Poll(material->frame);
		if (current && current->frame == material->frame)
			return;
		current.reset();
		const auto matches = [&](const auto& signal) {
			return signal && signal->material == material && signal->frame == material->frame &&
				signal->width == material->width && signal->height == material->height && signal->samples &&
				ValidLayout(signal->width, signal->height, signal->rowPitch, kLightingStride) &&
				signal->readyFence && ValidProducerSignal(signal->readyValue, signal->readyFence->GetCompletedValue());
		};
		if (!matches(direct)) direct.reset();
		if (!matches(indirect)) indirect.reset();
		const uint32_t available = (direct ? DirectLightingFrame::available : 0u) | (indirect ? indirect->available : 0u);
		const uint64_t bytes = uint64_t(material->width) * material->height * kLightingStride;
		if (!available || !bytes || bytes > UINT32_MAX)
			return;

		std::shared_ptr<FullNativeReconstructionFrame>* selected = nullptr;
		bool submitted = false;
		try {
			Initialize(backend);
			for (auto& slot : slots) {
				if (slot && slot->readyValue == UINT64_MAX && completion->GetCompletedValue() != UINT64_MAX) {
					const auto rescue = nextValue + 1;
					if (SUCCEEDED(owner.commandQueue->Signal(completion.get(), rescue))) {
						nextValue = rescue;
						slot->readyValue = rescue;
					}
				}
				if (!slot || (slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue))) {
					if (slot) {
						slot->material.reset();
						slot->direct.reset();
						slot->indirect.reset();
					}
					if (!selected || (slot && slot->width == material->width && slot->height == material->height))
						selected = &slot;
				}
			}
			if (!selected || completion->GetCompletedValue() == UINT64_MAX)
				return;
			auto& slot = *selected;
			auto* device = backend.d3d12Device.get();
			const bool fresh = !slot || slot->width != material->width || slot->height != material->height;
			if (fresh) {
				const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
				if (allocation == UINT64_MAX || allocation > budget.Query(device, 0).effectiveBytes)
					return;
				auto replacement = std::make_shared<FullNativeReconstructionFrame>();
				replacement->samples = Eval::Buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				replacement->allocationBytes = allocation;
				DX::ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(replacement->allocator.put())));
				DX::ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, replacement->allocator.get(), pipeline.get(), IID_PPV_ARGS(replacement->commands.put())));
				slot = std::move(replacement);
			} else {
				DX::ThrowIfFailed(slot->allocator->Reset());
				DX::ThrowIfFailed(slot->commands->Reset(slot->allocator.get(), pipeline.get()));
			}
			slot->material = std::move(material);
			slot->direct = std::move(direct);
			slot->indirect = std::move(indirect);
			slot->frame = slot->material->frame;
			slot->width = slot->material->width;
			slot->height = slot->material->height;
			slot->rowPitch = slot->width * kLightingStride;
			slot->available = available;
			slot->root = root;
			slot->pipeline = pipeline;
			slot->readyFence = completion;
			slot->readyValue = UINT64_MAX;
			auto* cmd = slot->commands.get();
			auto* depth = slot->material->raster->depth->resource.get();
			const auto address = slot->material->samples->GetGPUVirtualAddress();
			ID3D12DescriptorHeap* heaps[]{ slot->material->descriptors.get() };
			cmd->SetDescriptorHeaps(1, heaps);
			cmd->SetComputeRootSignature(root.get());
			cmd->SetComputeRootShaderResourceView(0, address);
			cmd->SetComputeRootShaderResourceView(1, slot->direct ? slot->direct->samples->GetGPUVirtualAddress() : address);
			cmd->SetComputeRootShaderResourceView(2, slot->indirect ? slot->indirect->samples->GetGPUVirtualAddress() : address);
			const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			cmd->SetComputeRootDescriptorTable(3, CD3DX12_GPU_DESCRIPTOR_HANDLE(slot->material->descriptors->GetGPUDescriptorHandleForHeapStart(), NativePBR::kGpuTextureDescriptorCapacity, increment));
			cmd->SetComputeRootUnorderedAccessView(4, slot->samples->GetGPUVirtualAddress());
			const uint32_t constants[]{ slot->width, slot->height, slot->material->rowPitch,
				slot->direct ? slot->direct->rowPitch : 0u, slot->indirect ? slot->indirect->rowPitch : 0u, available };
			cmd->SetComputeRoot32BitConstants(5, 6, constants, 0);
			const D3D12_RESOURCE_BARRIER begin[]{
				CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(slot->samples.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			};
			cmd->ResourceBarrier(2, begin);
			cmd->Dispatch((slot->width + 7) / 8, (slot->height + 7) / 8, 1);
			const D3D12_RESOURCE_BARRIER end[]{
				CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(slot->samples.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)
			};
			cmd->ResourceBarrier(2, end);
			DX::ThrowIfFailed(cmd->Close());
			DX::ThrowIfFailed(backend.commandQueue->Wait(slot->material->producerFence.get(), slot->material->producerValue));
			if (slot->direct) DX::ThrowIfFailed(backend.commandQueue->Wait(slot->direct->readyFence.get(), slot->direct->readyValue));
			if (slot->indirect) DX::ThrowIfFailed(backend.commandQueue->Wait(slot->indirect->readyFence.get(), slot->indirect->readyValue));
			ID3D12CommandList* lists[]{ cmd };
			backend.commandQueue->ExecuteCommandLists(1, lists);
			submitted = true;
			const auto value = nextValue + 1;
			DX::ThrowIfFailed(backend.commandQueue->Signal(completion.get(), value));
			nextValue = value;
			slot->readyValue = value;
			current = slot;
			failureLogged = false;
		} catch (const std::exception& error) {
			if (selected && *selected && !submitted)
				selected->reset();
			if (!failureLogged) {
				logger::warn("[NativeComposite] Full Native reconstruction unavailable: {}", error.what());
				failureLogged = true;
			}
		}
	}
}

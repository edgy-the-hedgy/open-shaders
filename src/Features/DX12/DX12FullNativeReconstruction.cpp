#include "DX12FullNativeReconstruction.h"
#include "DX12GpuRetirement.h"
#include "Utils/FileSystem.h"

#include <directx/d3dx12.h>
#include <cstring>

namespace DX12Native
{
	using Eval = NativeMaterialEvaluation;
	using namespace MaterialFrameContract;

	FullNativeReconstruction::~FullNativeReconstruction()
	{
		current.reset();
		history.reset();
		for (auto& slot : slots) {
			if (!slot || GpuFenceRetired(slot->readyFence.get(), slot->readyValue))
				continue;
			std::vector<FenceDependency> dependencies{ FencePoint(slot->readyFence.get(), slot->readyValue) };
			RetireAfterFences(std::move(slot), owner.d3d12Device, std::move(dependencies));
		}
	}

	void FullNativeReconstruction::Poll(uint32_t frame)
	{
		if (current && current->frame != frame) {
			history = frame - current->frame == 1u ? current : nullptr;
			current.reset();
		}
		if (history && frame - history->frame != 1u)
			history.reset();
		// A completed consumer no longer needs its predecessor. Break history
		// chains before choosing reusable slots, even if the latest frame is held.
		for (auto& slot : slots) {
			if (!slot)
				continue;
			if (slot->readyValue == UINT64_MAX && completion && owner.commandQueue &&
				completion->GetCompletedValue() != UINT64_MAX) {
				const auto rescue = nextValue + 1;
				if (SUCCEEDED(owner.commandQueue->Signal(completion.get(), rescue))) {
					nextValue = rescue;
					slot->readyValue = rescue;
				}
			}
			if (GpuFenceRetired(slot->readyFence.get(), slot->readyValue))
				slot->historyInput.reset();
		}
		for (auto& slot : slots) {
			if (slot && slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue)) {
				slot->material.reset();
				slot->direct.reset();
				slot->indirect.reset();
				slot->rasterEvidence.reset();
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
		const auto shader = Util::FileHelpers::ReadBoundedBinary(Util::PathHelpers::GetShadersPath() / "NativeComposite" / "FullNativeReconstruction.dxil", 16 * VideoMemoryBudget::kMiB);
		CD3DX12_DESCRIPTOR_RANGE depthRanges[3];
		depthRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0, 0);
		depthRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6, 0, 1);
		depthRanges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 9, 0, 2);
		CD3DX12_ROOT_PARAMETER parameters[12];
		parameters[0].InitAsShaderResourceView(0);
		parameters[1].InitAsShaderResourceView(1);
		parameters[2].InitAsShaderResourceView(2);
		parameters[3].InitAsDescriptorTable(3, depthRanges);
		parameters[4].InitAsUnorderedAccessView(0);
		parameters[5].InitAsConstants(17, 0);
		parameters[6].InitAsShaderResourceView(4);
		parameters[7].InitAsShaderResourceView(5);
		parameters[8].InitAsShaderResourceView(7);
		parameters[9].InitAsShaderResourceView(8);
		parameters[10].InitAsShaderResourceView(10);
		parameters[11].InitAsUnorderedAccessView(1);
		CD3DX12_ROOT_SIGNATURE_DESC signature(12, parameters);
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
		std::shared_ptr<const DirectLightingFrame> direct, std::shared_ptr<const IndirectLightingFrame> indirect,
		uint32_t pluginFrame, bool resetHistory, ID3D11Texture2D* rasterTokens,
		RasterIdentityStamp rasterStamp, std::span<const RasterPixelIdentity> tokens)
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
		const uint32_t quality = direct ? direct->quality : (indirect ? indirect->quality : kShadowQualityQuality);
		if (indirect && indirect->quality != quality) indirect.reset();
		const uint32_t available = (direct ? DirectLightingFrame::available : 0u) | (indirect ? indirect->available : 0u) |
			uint32_t(Emission);
		const uint64_t bytes = uint64_t(material->width) * material->height * kLightingStride;
		if (!available || !bytes || bytes > UINT32_MAX)
			return;
		const RasterIdentityStamp rayStamp{ material->frame, material->scope, material->geometryRevision, material->materialRevision };
		const StationaryHistoryIdentity stamp{ material->frame, pluginFrame, material->width, material->height,
			material->scope, available, material->geometryRevision, material->materialRevision,
			indirect && indirect->environment ? indirect->environment->revision : 0u, resetHistory };
		const auto sameCamera = [](const auto& a, const auto& b) {
			return std::memcmp(&a.CameraProjInverse, &b.CameraProjInverse, sizeof(a.CameraProjInverse)) == 0 &&
				std::memcmp(&a.CameraViewInverse, &b.CameraViewInverse, sizeof(a.CameraViewInverse)) == 0 &&
				std::memcmp(&a.CameraPosAdjust, &b.CameraPosAdjust, sizeof(a.CameraPosAdjust)) == 0;
		};
		if (!history || !history->material || !history->readyFence || history->quality != quality || !stamp.CanReuse(history->historyIdentity) ||
			!ValidProducerSignal(history->readyValue, history->readyFence->GetCompletedValue()) ||
			!history->material->IsCoherent(history->frame, history->width, history->height) ||
			history->material->scene != material->scene ||
			!sameCamera(history->material->raster->camera, material->raster->camera))
			history.reset();

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
					slot->rasterEvidence.reset();
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
				const auto maskDesc = CD3DX12_RESOURCE_DESC::Buffer(bytes / 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				const auto sampleAllocation = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
				const auto maskAllocation = device->GetResourceAllocationInfo(0, 1, &maskDesc).SizeInBytes;
				if (sampleAllocation == UINT64_MAX || maskAllocation == UINT64_MAX || sampleAllocation > UINT64_MAX - maskAllocation ||
					sampleAllocation + maskAllocation > budget.Query(device, 0).effectiveBytes)
					return;
				auto replacement = std::make_shared<FullNativeReconstructionFrame>();
				replacement->samples = Eval::Buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				replacement->ownershipMask = Eval::Buffer(device, bytes / 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				replacement->allocationBytes = sampleAllocation + maskAllocation;
				const D3D12_DESCRIPTOR_HEAP_DESC heap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3,
					D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
				DX::ThrowIfFailed(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(replacement->depthDescriptors.put())));
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
			// Selected lights and environment-only indirect are not complete raster coverage.
			slot->completeLightingCoverage = slot->direct && slot->indirect &&
				slot->direct->coverage == LightingCoverage::CompleteRasterReplacement &&
				slot->indirect->coverage == LightingCoverage::CompleteRasterReplacement &&
				(available & kOpaqueRequired) == kOpaqueRequired;
			if (rayStamp == rasterStamp) {
				const auto& evidence = slot->material->rawRasterEvidence;
				if (evidence && evidence->stamp == rasterStamp && evidence->tokenCount == tokens.size() &&
					evidence->width >= slot->width && evidence->height >= slot->height &&
					evidence->texture && evidence->texture->resource && evidence->tokenTable &&
					evidence->readyFence && ValidProducerSignal(evidence->readyValue, evidence->readyFence->GetCompletedValue()))
					slot->rasterEvidence = evidence;
				else
					slot->rasterEvidence = evidenceCapture.Capture(backend, rasterTokens, rasterStamp, tokens);
			}
			if (slot->rasterEvidence && (slot->rasterEvidence->width < slot->width || slot->rasterEvidence->height < slot->height))
				slot->rasterEvidence.reset();
			slot->historyIdentity = stamp;
			slot->quality = quality;
			slot->historyInput = std::move(history);
			slot->reconstruction = slot->historyInput ? ReconstructionState::Temporal : ReconstructionState::Spatial;
			slot->root = root;
			slot->pipeline = pipeline;
			slot->readyFence = completion;
			slot->readyValue = UINT64_MAX;
			auto* cmd = slot->commands.get();
			auto* depth = slot->material->raster->depth->resource.get();
			const auto address = slot->material->samples->GetGPUVirtualAddress();
			const auto* previous = slot->historyInput.get();
			auto* previousDepth = previous ? previous->material->raster->depth->resource.get() : depth;
			const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			const auto cpu = slot->depthDescriptors->GetCPUDescriptorHandleForHeapStart();
			D3D12_SHADER_RESOURCE_VIEW_DESC depthView{};
			depthView.Format = DXGI_FORMAT_R32_FLOAT;
			depthView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			depthView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			depthView.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(depth, &depthView, cpu);
			device->CreateShaderResourceView(previousDepth, &depthView, { cpu.ptr + SIZE_T(increment) });
			const auto evidence = slot->rasterEvidence;
			slot->rasterWinnerCertified = evidence && evidence->winnerCertified;
			auto* evidenceTexture = evidence ? evidence->texture->resource.get() : nullptr;
			auto tokenView = depthView;
			tokenView.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
			device->CreateShaderResourceView(evidenceTexture, &tokenView, { cpu.ptr + SIZE_T(2) * increment });
			ID3D12DescriptorHeap* heaps[]{ slot->depthDescriptors.get() };
			cmd->SetDescriptorHeaps(1, heaps);
			cmd->SetComputeRootSignature(root.get());
			cmd->SetComputeRootShaderResourceView(0, address);
			cmd->SetComputeRootShaderResourceView(1, slot->direct ? slot->direct->samples->GetGPUVirtualAddress() : address);
			cmd->SetComputeRootShaderResourceView(2, slot->indirect ? slot->indirect->samples->GetGPUVirtualAddress() : address);
			cmd->SetComputeRootDescriptorTable(3, slot->depthDescriptors->GetGPUDescriptorHandleForHeapStart());
			cmd->SetComputeRootShaderResourceView(6, previous ? previous->samples->GetGPUVirtualAddress() : address);
			cmd->SetComputeRootShaderResourceView(7, previous ? previous->material->samples->GetGPUVirtualAddress() : address);
			cmd->SetComputeRootShaderResourceView(8, slot->material->rayIdentity->GetGPUVirtualAddress());
			cmd->SetComputeRootShaderResourceView(9, previous ? previous->material->rayIdentity->GetGPUVirtualAddress() : slot->material->rayIdentity->GetGPUVirtualAddress());
			cmd->SetComputeRootUnorderedAccessView(4, slot->samples->GetGPUVirtualAddress());
			cmd->SetComputeRootShaderResourceView(10, evidence ? evidence->tokenTable->GetGPUVirtualAddress() : slot->material->rayIdentity->GetGPUVirtualAddress());
			cmd->SetComputeRootUnorderedAccessView(11, slot->ownershipMask->GetGPUVirtualAddress());
			const uint32_t constants[]{ slot->width, slot->height, slot->material->rowPitch,
				slot->direct ? slot->direct->rowPitch : 0u, slot->indirect ? slot->indirect->rowPitch : 0u, available,
				previous ? previous->material->rowPitch : 0u, previous ? 1u : 0u,
				slot->material->rayIdentityRowPitch, previous ? previous->material->rayIdentityRowPitch : 0u, evidence ? evidence->tokenCount : 0u,
				slot->rasterWinnerCertified ? 1u : 0u, slot->completeLightingCoverage ? 1u : 0u,
				slot->direct ? DirectLightingFrame::available : 0u, slot->indirect ? slot->indirect->available : 0u, quality, slot->frame };
			cmd->SetComputeRoot32BitConstants(5, 17, constants, 0);
			const D3D12_RESOURCE_BARRIER begin[]{
				CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(slot->samples.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				CD3DX12_RESOURCE_BARRIER::Transition(slot->ownershipMask.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
			};
			cmd->ResourceBarrier(3, begin);
			if (evidenceTexture) {
				const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(evidenceTexture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				cmd->ResourceBarrier(1, &barrier);
			}
			if (previousDepth != depth) {
				const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(previousDepth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				cmd->ResourceBarrier(1, &barrier);
			}
			cmd->Dispatch((slot->width + 7) / 8, (slot->height + 7) / 8, 1);
			const D3D12_RESOURCE_BARRIER end[]{
				CD3DX12_RESOURCE_BARRIER::Transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(slot->samples.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(slot->ownershipMask.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)
			};
			cmd->ResourceBarrier(3, end);
			if (evidenceTexture) {
				const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(evidenceTexture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
				cmd->ResourceBarrier(1, &barrier);
			}
			if (previousDepth != depth) {
				const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(previousDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
				cmd->ResourceBarrier(1, &barrier);
			}
			DX::ThrowIfFailed(cmd->Close());
			DX::ThrowIfFailed(backend.commandQueue->Wait(slot->material->producerFence.get(), slot->material->producerValue));
			if (slot->direct) DX::ThrowIfFailed(backend.commandQueue->Wait(slot->direct->readyFence.get(), slot->direct->readyValue));
			if (slot->indirect) DX::ThrowIfFailed(backend.commandQueue->Wait(slot->indirect->readyFence.get(), slot->indirect->readyValue));
			if (previous) DX::ThrowIfFailed(backend.commandQueue->Wait(previous->readyFence.get(), previous->readyValue));
			if (evidence) DX::ThrowIfFailed(backend.commandQueue->Wait(evidence->readyFence.get(), evidence->readyValue));
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

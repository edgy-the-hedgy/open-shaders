#include "DX12FullNativeFinalColor.h"
#include "DX12GpuRetirement.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Util.h"
#include "Utils/FileSystem.h"

#include <directx/d3dx12.h>

namespace DX12Native
{
	using namespace MaterialFrameContract;

	FullNativeFinalColor::~FullNativeFinalColor()
	{
		current.reset();
		for (auto& slot : slots) {
			if (!slot)
				continue;
			std::vector<FenceDependency> dependencies{
				FencePoint(slot->readyFence.get(), slot->readyValue),
				FencePoint(slot->consumedFence.get(), slot->consumedValue)
			};
			RetireAfterFences(std::move(slot), owner.d3d12Device, std::move(dependencies));
		}
	}

	void FullNativeFinalColor::Initialize(const GraphicsBackend& backend)
	{
		if (pipeline) {
			if (owner.d3d12Device.get() != backend.d3d12Device.get() ||
				owner.commandQueue.get() != backend.commandQueue.get() ||
				owner.d3d11Context.get() != backend.d3d11Context.get())
				throw std::runtime_error("Full Native final color backend changed");
			return;
		}
		auto* device = backend.d3d12Device.get();
		const auto shader = Util::FileHelpers::ReadBoundedBinary(Util::PathHelpers::GetShadersPath() / "NativeComposite" / "CompositeFullNative.dxil", 16 * VideoMemoryBudget::kMiB);
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
		CD3DX12_ROOT_PARAMETER parameters[5];
		parameters[0].InitAsShaderResourceView(0);
		parameters[1].InitAsShaderResourceView(1);
		parameters[2].InitAsShaderResourceView(2);
		parameters[3].InitAsDescriptorTable(1, &range);
		parameters[4].InitAsConstants(4, 0);
		CD3DX12_ROOT_SIGNATURE_DESC signature(5, parameters);
		winrt::com_ptr<ID3DBlob> blob, errors;
		winrt::com_ptr<ID3D12RootSignature> newRoot;
		winrt::com_ptr<ID3D12PipelineState> newPipeline;
		DX::ThrowIfFailed(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put()));
		DX::ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(newRoot.put())));
		D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
		pso.pRootSignature = newRoot.get();
		pso.CS = { shader.data(), shader.size() };
		DX::ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(newPipeline.put())));
		owner = backend;
		producer.Create(device, backend.d3d11Device.get(), "NativeComposite::FullNativeColorProducer");
		consumer.Create(device, backend.d3d11Device.get(), "NativeComposite::FullNativeColorConsumer");
		root = std::move(newRoot);
		pipeline = std::move(newPipeline);
	}

	void FullNativeFinalColor::Poll(uint32_t frame)
	{
		if (current && current->frame != frame)
			current.reset();
		for (auto& slot : slots) {
			if (!slot)
				continue;
			if (slot->readyValue == UINT64_MAX && producer.fence12 && owner.commandQueue &&
				producer.fence12->GetCompletedValue() != UINT64_MAX) {
				const auto rescue = producer.Next();
				if (SUCCEEDED(owner.commandQueue->Signal(producer.fence12.get(), rescue)))
					slot->readyValue = rescue;
			}
			if (slot->consumedValue == UINT64_MAX && consumer.fence12 && owner.d3d11Context &&
				consumer.fence12->GetCompletedValue() != UINT64_MAX) {
				const auto rescue = consumer.Next();
				if (SUCCEEDED(owner.d3d11Context->Signal(consumer.fence11.get(), rescue))) {
					owner.d3d11Context->Flush();
					slot->consumedValue = rescue;
				}
			}
			// Release the reconstruction chain before the next upstream
			// material/reconstruction dispatch chooses a slot.
			if (GpuFenceRetired(slot->readyFence.get(), slot->readyValue))
				slot->input.reset();
		}
	}

	std::shared_ptr<const FullNativeFinalColorFrame> FullNativeFinalColor::Get(uint32_t frame) const
	{
		return current && current->frame == frame && current->readyFence &&
			ValidProducerSignal(current->readyValue, current->readyFence->GetCompletedValue()) ? current : nullptr;
	}

	void FullNativeFinalColor::Compose(const GraphicsBackend& backend, std::shared_ptr<const FullNativeReconstructionFrame> input)
	{
		if (!input || !input->material || !input->historySignals || !input->ownershipMask || !input->readyFence ||
			!ValidProducerSignal(input->readyValue, input->readyFence->GetCompletedValue()) ||
			!input->material->IsCoherent(input->frame, input->width, input->height))
			return;
		// Current-frame independent reconstructed signals, not the previous history input.
		static_assert(FullNativeReconstructionFrame::historySignalStride == 6 * kOpaqueRadianceStride);
		const uint64_t pixels = uint64_t(input->width) * input->height;
		const auto signals = input->historySignals->GetDesc();
		const auto ownership = input->ownershipMask->GetDesc();
		if (!pixels || pixels > UINT32_MAX / FullNativeReconstructionFrame::historySignalStride ||
			signals.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
			signals.Width < pixels * FullNativeReconstructionFrame::historySignalStride ||
			ownership.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || ownership.Width < pixels * sizeof(uint32_t))
			return;
		Poll(input->frame);
		if (Get(input->frame))
			return;
		current.reset();
		std::shared_ptr<FullNativeFinalColorFrame>* selected = nullptr;
		bool submitted = false;
		try {
			Initialize(backend);
			for (auto& slot : slots) {
				if (!slot || (slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue) &&
						GpuFenceRetired(slot->consumedFence.get(), slot->consumedValue))) {
					if (!selected || (slot && slot->width == input->width && slot->height == input->height))
						selected = &slot;
				}
			}
			if (!selected || producer.fence12->GetCompletedValue() == UINT64_MAX || consumer.fence12->GetCompletedValue() == UINT64_MAX)
				return;
			auto& slot = *selected;
			auto* device = backend.d3d12Device.get();
			if (!slot || slot->width != input->width || slot->height != input->height) {
				constexpr auto format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, input->width, input->height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				const auto bytes = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
				if (bytes == UINT64_MAX || bytes > budget.Query(device, 0).effectiveBytes)
					return;
				auto replacement = std::make_shared<FullNativeFinalColorFrame>();
				D3D11_TEXTURE2D_DESC shared{};
				shared.Width = input->width;
				shared.Height = input->height;
				shared.MipLevels = shared.ArraySize = shared.SampleDesc.Count = 1;
				shared.Format = format;
				shared.Usage = D3D11_USAGE_DEFAULT;
				shared.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				replacement->color = std::make_unique<WrappedResource>(shared, backend.d3d11Device.get(), device, "NativeComposite::FullNativeFinalColor");
				const D3D12_DESCRIPTOR_HEAP_DESC heap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
				DX::ThrowIfFailed(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(replacement->descriptors.put())));
				D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
				view.Format = format;
				view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				device->CreateUnorderedAccessView(replacement->color->resource.get(), nullptr, &view, replacement->descriptors->GetCPUDescriptorHandleForHeapStart());
				DX::ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(replacement->allocator.put())));
				DX::ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, replacement->allocator.get(), pipeline.get(), IID_PPV_ARGS(replacement->commands.put())));
				slot = std::move(replacement);
			} else {
				DX::ThrowIfFailed(slot->allocator->Reset());
				DX::ThrowIfFailed(slot->commands->Reset(slot->allocator.get(), pipeline.get()));
			}
			slot->input = std::move(input);
			slot->frame = slot->input->frame;
			slot->width = slot->input->width;
			slot->height = slot->input->height;
			slot->applied = false;
			slot->root = root;
			slot->pipeline = pipeline;
			slot->readyFence = producer.fence12;
			slot->consumedFence = consumer.fence12;
			slot->readyValue = UINT64_MAX;
			slot->consumedValue = 0;
			auto* cmd = slot->commands.get();
			auto* texture = slot->color->resource.get();
			ID3D12DescriptorHeap* heaps[]{ slot->descriptors.get() };
			cmd->SetDescriptorHeaps(1, heaps);
			cmd->SetComputeRootSignature(root.get());
			cmd->SetComputeRootShaderResourceView(0, slot->input->material->samples->GetGPUVirtualAddress());
			cmd->SetComputeRootShaderResourceView(1, slot->input->historySignals->GetGPUVirtualAddress());
			cmd->SetComputeRootShaderResourceView(2, slot->input->ownershipMask->GetGPUVirtualAddress());
			cmd->SetComputeRootDescriptorTable(3, slot->descriptors->GetGPUDescriptorHandleForHeapStart());
			const uint32_t constants[]{ slot->width, slot->height, slot->input->material->rowPitch,
				slot->input->available };
			cmd->SetComputeRoot32BitConstants(4, 4, constants, 0);
			auto transition = CD3DX12_RESOURCE_BARRIER::Transition(texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			cmd->ResourceBarrier(1, &transition);
			cmd->Dispatch((slot->width + 7) / 8, (slot->height + 7) / 8, 1);
			transition = CD3DX12_RESOURCE_BARRIER::Transition(texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			cmd->ResourceBarrier(1, &transition);
			DX::ThrowIfFailed(cmd->Close());
			DX::ThrowIfFailed(owner.commandQueue->Wait(slot->input->readyFence.get(), slot->input->readyValue));
			ID3D12CommandList* lists[]{ cmd };
			owner.commandQueue->ExecuteCommandLists(1, lists);
			submitted = true;
			const auto ready = producer.Next();
			DX::ThrowIfFailed(owner.commandQueue->Signal(producer.fence12.get(), ready));
			slot->readyValue = ready;
			current = slot;
			failureLogged = false;
		} catch (...) {
			if (selected && *selected && !submitted)
				selected->reset();
			if (!failureLogged) {
				logger::warn("[NativeComposite] Full Native final color composition unavailable");
				failureLogged = true;
			}
		}
	}

	bool FullNativeFinalColor::Apply(uint32_t frame, ID3D11UnorderedAccessView* target)
	{
		if (!Get(frame) || !target || current->applied ||
			!globals::state || globals::state->isMapMenuOpen)
			return false;
		try {
			if (!applyShader) {
				const auto path = Util::PathHelpers::GetShadersPath() / "NativeComposite" / "ApplyFullNative.cs.hlsl";
				applyShader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), {}, "cs_5_0")));
			}
			if (!applyShader)
				return false;
			winrt::com_ptr<ID3D11Resource> resource;
			target->GetResource(resource.put());
			const auto texture = resource.try_as<ID3D11Texture2D>();
			D3D11_TEXTURE2D_DESC desc{};
			if (!texture)
				return false;
			texture->GetDesc(&desc);
			if (desc.Width < current->width || desc.Height < current->height || desc.ArraySize != 1 || desc.SampleDesc.Count != 1)
				return false;
			auto* context = owner.d3d11Context.get();
			DX::ThrowIfFailed(context->Wait(producer.fence11.get(), current->readyValue));
			ID3D11ShaderResourceView* srv = current->color->srv;
			context->CSSetShaderResources(0, 1, &srv);
			context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
			globals::state->BindSharedDataCS(context);
			context->CSSetShader(applyShader.get(), nullptr, 0);
			current->consumedValue = UINT64_MAX;
			context->Dispatch((current->width + 7) / 8, (current->height + 7) / 8, 1);
			current->applied = true;
			ID3D11ShaderResourceView* nullSrv = nullptr;
			ID3D11UnorderedAccessView* nullUav = nullptr;
			context->CSSetShaderResources(0, 1, &nullSrv);
			context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
			const auto consumed = consumer.Next();
			DX::ThrowIfFailed(context->Signal(consumer.fence11.get(), consumed));
			current->consumedValue = consumed;
			context->Flush();
			return true;
		} catch (...) {
			logger::warn("[NativeComposite] Full Native final color application unavailable");
			return false;
		}
	}
}

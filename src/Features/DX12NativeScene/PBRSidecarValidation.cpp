#include "PBRSidecarValidation.h"

#include "GpuPass.h"
#include "Globals.h"
#include "TruePBR.h"
#include "TruePBR/NativePBRCompiler.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <cstring>
#include <directx/d3dx12.h>
#include <fstream>
#include <ranges>
#include <stdexcept>

namespace DX12Native
{
	namespace
	{
		constexpr uint32_t kPreviewStep = 8;
		constexpr uint32_t kSrvCount = NativePBR::MaterialSidecar::kCount + 1;

		winrt::com_ptr<ID3D12Resource> Buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, uint64_t bytes,
			D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
		{
			winrt::com_ptr<ID3D12Resource> result;
			const auto properties = CD3DX12_HEAP_PROPERTIES(heap);
			const auto description = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
			DX::ThrowIfFailed(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
				&description, state, nullptr, IID_PPV_ARGS(result.put())));
			return result;
		}
	}

	PBRSidecarValidation::~PBRSidecarValidation()
	{
		const auto done = completion ? completion->GetCompletedValue() : 0;
		for (auto& slot : slots)
			if (slot.frame && (done == UINT64_MAX || slot.readyValue == UINT64_MAX || slot.readyValue > done))
				(void)new Slot(std::move(slot));
	}

	void PBRSidecarValidation::Reset()
	{
		latest = {};
		status.clear();
		lastSubmission = UINT32_MAX;
		++generation;
	}

	void PBRSidecarValidation::CreatePipeline(ID3D12Device* device)
	{
		if (pipeline)
			return;
		std::ifstream file("Data/Shaders/DX12NativeScene/PBRSidecarValidation.dxil", std::ios::binary | std::ios::ate);
		if (!file || file.tellg() <= 0 || file.tellg() > 16 * 1024 * 1024)
			throw std::runtime_error("NativePBR validation DXIL missing or invalid");
		std::vector<char> bytecode(static_cast<size_t>(file.tellg()));
		file.seekg(0);
		if (!file.read(bytecode.data(), std::streamsize(bytecode.size())))
			throw std::runtime_error("Cannot read NativePBR validation DXIL");
		D3D12_DESCRIPTOR_RANGE ranges[2]{};
		ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvCount, 0, 0, 0 };
		ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 0 };
		D3D12_ROOT_PARAMETER parameters[4]{};
		parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[0].Constants = { 0, 0, 8 };
		parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[1].DescriptorTable = { 1, &ranges[0] };
		parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
		parameters[2].Descriptor.ShaderRegister = 0;
		parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[3].DescriptorTable = { 1, &ranges[1] };
		const D3D12_ROOT_SIGNATURE_DESC description{ 4, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
		winrt::com_ptr<ID3DBlob> signature, errors;
		DX::ThrowIfFailed(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
			signature.put(), errors.put()));
		DX::ThrowIfFailed(device->CreateRootSignature(0, signature->GetBufferPointer(),
			signature->GetBufferSize(), IID_PPV_ARGS(root.put())));
		root->SetName(L"NativePBR::ValidationRoot");
		D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
		state.pRootSignature = root.get();
		state.CS = { bytecode.data(), bytecode.size() };
		DX::ThrowIfFailed(device->CreateComputePipelineState(&state, IID_PPV_ARGS(pipeline.put())));
		pipeline->SetName(L"NativePBR::SidecarValidation");
	}

	void PBRSidecarValidation::PrepareSlot(const GraphicsBackend& backend, Slot& slot, uint32_t width, uint32_t height)
	{
		const uint32_t previewW = (width + kPreviewStep - 1) / kPreviewStep;
		const uint32_t previewH = (height + kPreviewStep - 1) / kPreviewStep;
		if (slot.preview && slot.previewWidth == previewW && slot.previewHeight == previewH)
			return;
		slot = {};
		auto* device = backend.d3d12Device.get();
		D3D12_DESCRIPTOR_HEAP_DESC heap{};
		heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heap.NumDescriptors = kSrvCount + 1;
		heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		DX::ThrowIfFailed(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(slot.descriptors.put())));
		slot.report = Buffer(device, D3D12_HEAP_TYPE_DEFAULT, sizeof(Report), D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		slot.report->SetName(L"NativePBR::ValidationCounters");
		slot.zero = Buffer(device, D3D12_HEAP_TYPE_UPLOAD, sizeof(Report), D3D12_RESOURCE_STATE_GENERIC_READ);
		slot.readback = Buffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(Report), D3D12_RESOURCE_STATE_COPY_DEST);
		void* zero = nullptr;
		const D3D12_RANGE noRead{ 0, 0 };
		DX::ThrowIfFailed(slot.zero->Map(0, &noRead, &zero));
		std::memset(zero, 0, sizeof(Report));
		slot.zero->Unmap(0, nullptr);
		auto texture = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, previewW, previewH,
			1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		const auto properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		DX::ThrowIfFailed(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
			&texture, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(slot.preview.put())));
		slot.preview->SetName(L"NativePBR::ValidationPreview");
		device->GetCopyableFootprints(&texture, 0, 1, 0, &slot.previewFootprint, nullptr, nullptr,
			&slot.previewBytes);
		slot.previewReadback = Buffer(device, D3D12_HEAP_TYPE_READBACK, slot.previewBytes,
			D3D12_RESOURCE_STATE_COPY_DEST);
		DX::ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(slot.allocator.put())));
		DX::ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			slot.allocator.get(), pipeline.get(), IID_PPV_ARGS(slot.commands.put())));
		DX::ThrowIfFailed(slot.commands->Close());
		slot.previewWidth = previewW;
		slot.previewHeight = previewH;
	}

	void PBRSidecarValidation::Poll(const GraphicsBackend& backend)
	{
		if (!completion || !backend.d3d11Device || !backend.d3d11Context)
			return;
		const auto done = completion->GetCompletedValue();
		if (done == UINT64_MAX) {
			status = "NativePBR validation GPU device removed";
			return;
		}
		for (auto& slot : slots) {
			if (!slot.frame || slot.readyValue == UINT64_MAX || slot.readyValue > done)
				continue;
			if (slot.generation != generation) {
				slot.frame.reset();
				continue;
			}
			try {
				if (!latest.valid || slot.frame->frame >= latest.frame) {
					Result result{};
					result.frame = slot.frame->frame;
					result.width = slot.frame->width;
					result.height = slot.frame->height;
					result.inspectedX = slot.inspectedX;
					result.inspectedY = slot.inspectedY;
					const D3D12_RANGE range{ 0, sizeof(Report) };
					void* values = nullptr;
					DX::ThrowIfFailed(slot.readback->Map(0, &range, &values));
					std::memcpy(&result.report, values, sizeof(Report));
					const D3D12_RANGE noWrite{ 0, 0 };
					slot.readback->Unmap(0, &noWrite);
					if (slot.frame->pbrDraws)
						result.draws = *slot.frame->pbrDraws;
					if (const auto recipes = globals::features::truePBR.GetNativeRecipes())
						for (const auto& draw : result.draws) {
							const auto entry = recipes->recipes.find(draw.materialIdentity);
							if (entry == recipes->recipes.end())
								++result.missingRecipes;
							else if (entry->second->revision != draw.materialRevision)
								++result.revisionMismatches;
						}
					else
						result.missingRecipes = uint32_t(result.draws.size());
					if (!preview11 || previewWidth != slot.previewWidth || previewHeight != slot.previewHeight) {
						previewSRV = nullptr;
						preview11 = nullptr;
						D3D11_TEXTURE2D_DESC desc{};
						desc.Width = slot.previewWidth;
						desc.Height = slot.previewHeight;
						desc.MipLevels = desc.ArraySize = 1;
						desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
						desc.SampleDesc.Count = 1;
						desc.Usage = D3D11_USAGE_DYNAMIC;
						desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
						desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
						DX::ThrowIfFailed(backend.d3d11Device->CreateTexture2D(&desc, nullptr, preview11.put()));
						Util::SetResourceName(preview11.get(), "NativePBR::PreviewUpload");
						DX::ThrowIfFailed(backend.d3d11Device->CreateShaderResourceView(preview11.get(), nullptr,
							previewSRV.put()));
						Util::SetResourceName(previewSRV.get(), "NativePBR::PreviewUpload SRV");
							previewWidth = slot.previewWidth;
						previewHeight = slot.previewHeight;
					}
					void* pixels = nullptr;
					const D3D12_RANGE imageRange{ 0, SIZE_T(slot.previewBytes) };
					DX::ThrowIfFailed(slot.previewReadback->Map(0, &imageRange, &pixels));
					D3D11_MAPPED_SUBRESOURCE upload{};
					const HRESULT mapResult = backend.d3d11Context->Map(preview11.get(), 0,
						D3D11_MAP_WRITE_DISCARD, 0, &upload);
					if (SUCCEEDED(mapResult)) {
						for (uint32_t row = 0; row < previewHeight; ++row)
							std::memcpy(static_cast<uint8_t*>(upload.pData) + row * upload.RowPitch,
								static_cast<uint8_t*>(pixels) + row * slot.previewFootprint.Footprint.RowPitch,
								size_t(previewWidth) * 4);
						backend.d3d11Context->Unmap(preview11.get(), 0);
					} else
						status = "NativePBR preview upload unavailable";
					slot.previewReadback->Unmap(0, &noWrite);
					result.valid = true;
					latest = std::move(result);
				}
			} catch (const std::exception& error) {
				status = error.what();
			}
			slot.frame.reset();
		}
	}

	void PBRSidecarValidation::Advance(const GraphicsBackend& backend, std::shared_ptr<const RasterFrame> frame)
	{
		CS_GPU_PASS("NativePBR::ValidateCopiedSidecar");
		if (ownerDevice && (ownerDevice.get() != backend.d3d12Device.get() || ownerQueue.get() != backend.commandQueue.get())) {
			status = "NativePBR validation backend changed; restart required";
			return;
		}
		Poll(backend);
		if (!enabled || !frame || !frame->pbrSidecarValid || !frame->pbrDraws ||
			frame->pbrDraws->empty() || !frame->readyFence || !frame->readyValue)
			return;
		if (lastSubmission != UINT32_MAX && frame->frame - lastSubmission < std::max(1u, intervalFrames))
			return;
		if (frame->width > 8192 || frame->height > 8192 || frame->pbrDraws->size() > UINT32_MAX) {
			status = "NativePBR validation dimensions or draw count unsupported";
			return;
		}
		auto* device = backend.d3d12Device.get();
		const auto done = completion ? completion->GetCompletedValue() : 0;
		if (done == UINT64_MAX) {
			status = "NativePBR validation device removed";
			return;
		}
		const auto available = std::ranges::find_if(slots, [done](const Slot& item) {
			return !item.frame || (item.readyValue != UINT64_MAX && item.readyValue <= done);
		});
		if (available == slots.end())
			return;
		try {
			if (!device || !backend.commandQueue)
				throw std::runtime_error("NativePBR validation backend unavailable");
			if (!ownerDevice) {
				ownerDevice.copy_from(device);
				ownerQueue.copy_from(backend.commandQueue.get());
			}
			CreatePipeline(device);
			if (!completion)
				DX::ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
					IID_PPV_ARGS(completion.put())));
			auto& slot = *available;
			PrepareSlot(backend, slot, frame->width, frame->height);
			const uint32_t size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			auto cpu = slot.descriptors->GetCPUDescriptorHandleForHeapStart();
			for (uint32_t i = 0; i < kSrvCount; ++i) {
				ID3D12Resource* resource = i == kSrvCount - 1 ? frame->depth->resource.get() :
					frame->pbrSidecar[i]->resource.get();
				D3D12_SHADER_RESOURCE_VIEW_DESC view{};
				view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				view.Format = resource->GetDesc().Format;
				view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				view.Texture2D.MipLevels = 1;
				device->CreateShaderResourceView(resource, &view, cpu);
				cpu.ptr += size;
			}
			D3D12_UNORDERED_ACCESS_VIEW_DESC previewView{};
			previewView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			previewView.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			device->CreateUnorderedAccessView(slot.preview.get(), nullptr, &previewView, cpu);
			DX::ThrowIfFailed(slot.allocator->Reset());
			DX::ThrowIfFailed(slot.commands->Reset(slot.allocator.get(), pipeline.get()));
			auto* commands = slot.commands.get();
			commands->CopyBufferRegion(slot.report.get(), 0, slot.zero.get(), 0, sizeof(Report));
			std::vector<D3D12_RESOURCE_BARRIER> begin;
			begin.reserve(kSrvCount + 2);
			for (uint32_t i = 0; i < kSrvCount; ++i) {
				auto* resource = i == kSrvCount - 1 ? frame->depth->resource.get() :
					frame->pbrSidecar[i]->resource.get();
				begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource, D3D12_RESOURCE_STATE_COMMON,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
			}
			begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(slot.report.get(), D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
			begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(slot.preview.get(), D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
			commands->ResourceBarrier(UINT(begin.size()), begin.data());
			ID3D12DescriptorHeap* heaps[]{ slot.descriptors.get() };
			commands->SetDescriptorHeaps(1, heaps);
			commands->SetComputeRootSignature(root.get());
			commands->SetPipelineState(pipeline.get());
			const std::array<uint32_t, 8> constants{ frame->width, frame->height,
				uint32_t(frame->pbrDraws->size()), std::min(inspectX, frame->width - 1),
				std::min(inspectY, frame->height - 1), kPreviewStep, std::min(mode, 4u),
				frame->sourceDepthFormat == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ? 1u : 0u };
			commands->SetComputeRoot32BitConstants(0, 8, constants.data(), 0);
			commands->SetComputeRootDescriptorTable(1, slot.descriptors->GetGPUDescriptorHandleForHeapStart());
			commands->SetComputeRootUnorderedAccessView(2, slot.report->GetGPUVirtualAddress());
			auto previewGpu = slot.descriptors->GetGPUDescriptorHandleForHeapStart();
			previewGpu.ptr += uint64_t(kSrvCount) * size;
			commands->SetComputeRootDescriptorTable(3, previewGpu);
			commands->Dispatch((frame->width + 7) / 8, (frame->height + 7) / 8, 1);
			std::array ordered{ CD3DX12_RESOURCE_BARRIER::UAV(slot.report.get()),
				CD3DX12_RESOURCE_BARRIER::UAV(slot.preview.get()) };
			commands->ResourceBarrier(UINT(ordered.size()), ordered.data());
			std::array copy{
				CD3DX12_RESOURCE_BARRIER::Transition(slot.report.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_COPY_SOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(slot.preview.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_COPY_SOURCE)
			};
			commands->ResourceBarrier(UINT(copy.size()), copy.data());
			commands->CopyBufferRegion(slot.readback.get(), 0, slot.report.get(), 0, sizeof(Report));
			const CD3DX12_TEXTURE_COPY_LOCATION source(slot.preview.get(), 0);
			const CD3DX12_TEXTURE_COPY_LOCATION target(slot.previewReadback.get(), slot.previewFootprint);
			commands->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
			std::vector<D3D12_RESOURCE_BARRIER> end;
			end.reserve(kSrvCount + 2);
			for (uint32_t i = 0; i < kSrvCount; ++i) {
				auto* resource = i == kSrvCount - 1 ? frame->depth->resource.get() :
					frame->pbrSidecar[i]->resource.get();
				end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource,
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
			}
			end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(slot.report.get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_COPY_DEST));
			end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(slot.preview.get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_COMMON));
			commands->ResourceBarrier(UINT(end.size()), end.data());
			DX::ThrowIfFailed(commands->Close());
			DX::ThrowIfFailed(backend.commandQueue->Wait(frame->readyFence.get(), frame->readyValue));
			ID3D12CommandList* lists[]{ commands };
			slot.frame = std::move(frame);
			slot.generation = generation;
			slot.readyValue = UINT64_MAX;
			backend.commandQueue->ExecuteCommandLists(1, lists);
			const uint64_t signal = ++nextFence;
			DX::ThrowIfFailed(backend.commandQueue->Signal(completion.get(), signal));
			slot.readyValue = signal;
			slot.inspectedX = constants[3];
			slot.inspectedY = constants[4];
			lastSubmission = slot.frame->frame;
			status.clear();
		} catch (const std::exception& error) {
			status = error.what();
			if (!available->frame)
				*available = {};
		}
	}
}

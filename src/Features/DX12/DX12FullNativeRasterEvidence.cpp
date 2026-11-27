#include "DX12FullNativeRasterEvidence.h"
#include "DX12GpuRetirement.h"
#include "DX12NativeMaterialEvaluation.h"
#include "GpuPass.h"
#include <directx/d3dx12.h>
#include <cstring>

namespace DX12Native
{
	using namespace MaterialFrameContract;

	FullNativeRasterEvidenceCapture::~FullNativeRasterEvidenceCapture()
	{
		for (auto& slot : slots) {
			if (!slot || GpuFenceRetired(slot->readyFence.get(), slot->readyValue))
				continue;
			std::vector<FenceDependency> dependencies{ FencePoint(slot->readyFence.get(), slot->readyValue) };
			RetireAfterFences(std::move(slot), owner.d3d12Device, std::move(dependencies));
		}
	}

	std::shared_ptr<const FullNativeRasterEvidence> FullNativeRasterEvidenceCapture::Capture(
		const GraphicsBackend& backend, ID3D11Texture2D* source, RasterIdentityStamp stamp,
		std::span<const RasterPixelIdentity> tokens)
	{
		CS_GPU_PASS("NativeComposite::CaptureFullNativeRasterEvidence");
		if (!source || tokens.empty() || tokens.size() > kMaxRasterPrimitiveToken ||
			!stamp.scope || !stamp.geometryRevision || !stamp.materialRevision ||
			!backend.d3d11Device || !backend.d3d11Context || !backend.d3d12Device || !backend.commandQueue)
			return {};
		std::shared_ptr<FullNativeRasterEvidence>* selected = nullptr;
		bool copied = false;
		try {
			winrt::com_ptr<ID3D11Device> sourceDevice;
			source->GetDevice(sourceDevice.put());
			D3D11_TEXTURE2D_DESC desc{};
			source->GetDesc(&desc);
			if (sourceDevice.get() != backend.d3d11Device.get() || desc.Format != DXGI_FORMAT_R16G16B16A16_UNORM ||
				!desc.Width || !desc.Height || desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1)
				return {};
			if (!producer.fence12) {
				owner = backend;
				producer.Create(owner.d3d12Device.get(), owner.d3d11Device.get(), "NativeComposite::FullNativeRasterEvidence");
			}
			if (owner.d3d11Device.get() != backend.d3d11Device.get() ||
				owner.d3d12Device.get() != backend.d3d12Device.get() ||
				owner.commandQueue.get() != backend.commandQueue.get())
				return {};
			if (producer.fence12->GetCompletedValue() == UINT64_MAX)
				return {};
			for (auto& slot : slots) {
				if (slot && slot->readyValue == UINT64_MAX) {
					const auto rescue = producer.Next();
					if (SUCCEEDED(owner.d3d11Context->Signal(producer.fence11.get(), rescue))) {
						owner.d3d11Context->Flush();
						slot->readyValue = rescue;
					}
				}
				if (!slot || (slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue))) {
					if (!selected || (slot && slot->width == desc.Width && slot->height == desc.Height))
						selected = &slot;
				}
			}
			if (!selected)
				return {};
			auto& slot = *selected;
			auto* device = owner.d3d12Device.get();
			constexpr uint64_t tableBytes = uint64_t(kMaxRasterPrimitiveToken) * sizeof(RasterPixelIdentity);
			if (!slot || slot->width != desc.Width || slot->height != desc.Height) {
				const auto textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(desc.Format, desc.Width, desc.Height, 1, 1);
				const auto tableDesc = CD3DX12_RESOURCE_DESC::Buffer(tableBytes);
				const auto textureBytes = device->GetResourceAllocationInfo(0, 1, &textureDesc).SizeInBytes;
				const auto uploadBytes = device->GetResourceAllocationInfo(0, 1, &tableDesc).SizeInBytes;
				if (textureBytes == UINT64_MAX || uploadBytes == UINT64_MAX || textureBytes > UINT64_MAX - uploadBytes ||
					textureBytes + uploadBytes > budget.Query(device, 0).effectiveBytes)
					return {};
				auto replacement = std::make_shared<FullNativeRasterEvidence>();
				replacement->allocationBytes = textureBytes + uploadBytes;
				replacement->width = desc.Width;
				replacement->height = desc.Height;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.CPUAccessFlags = desc.MiscFlags = 0;
				replacement->texture = std::make_unique<WrappedResource>(desc, owner.d3d11Device.get(), device,
					"NativeComposite::FullNativeRasterEvidence");
				replacement->tokenTable = NativeMaterialEvaluation::Buffer(device, tableBytes,
					D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
				slot = std::move(replacement);
			}
			void* mapped = nullptr;
			const D3D12_RANGE noRead{};
			DX::ThrowIfFailed(slot->tokenTable->Map(0, &noRead, &mapped));
			std::memcpy(mapped, tokens.data(), tokens.size_bytes());
			const D3D12_RANGE written{ 0, tokens.size_bytes() };
			slot->tokenTable->Unmap(0, &written);
			slot->stamp = stamp;
			slot->tokenCount = uint32_t(tokens.size());
			slot->readyFence = producer.fence12;
			slot->readyValue = UINT64_MAX;
			owner.d3d11Context->CopyResource(slot->texture->resource11, source);
			copied = true;
			const auto ready = producer.Next();
			DX::ThrowIfFailed(owner.d3d11Context->Signal(producer.fence11.get(), ready));
			slot->readyValue = ready;
			owner.d3d11Context->Flush();
			failureLogged = false;
			return slot;
		} catch (...) {
			if (selected && *selected && !copied)
				selected->reset();
			if (!failureLogged) {
				logger::warn("[NativeComposite] Full Native raster evidence capture unavailable");
				failureLogged = true;
			}
			return {};
		}
	}
}

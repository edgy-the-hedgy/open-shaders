#include "NativeRawInputCapture.h"

#include "Features/DX12/DX12GpuRetirement.h"
#include "Globals.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <directx/d3dx12.h>

namespace NativePBR
{
	using namespace DX12Native;
	using namespace MaterialFrameContract;
	namespace
	{
		constexpr auto kFormat = DXGI_FORMAT_R32G32B32A32_UINT;
	}

	bool RawInputFrame::Matches(RasterIdentityStamp requested, uint32_t viewWidth, uint32_t viewHeight) const
	{
		return stamp.frame == requested.frame && stamp.scope == requested.scope &&
			stamp.geometryRevision == requested.geometryRevision &&
			(!stamp.materialRevision || stamp.materialRevision == requested.materialRevision) && width >= viewWidth && height >= viewHeight && draws &&
			std::ranges::all_of(planes, [](const auto& plane) { return plane && plane->resource; }) &&
			readyFence && ValidProducerSignal(readyValue, readyFence->GetCompletedValue()) &&
			evidence && evidence->stamp == stamp && evidence->width == width && evidence->height == height &&
			evidence->texture && evidence->texture->resource &&
			evidence->readyFence && ValidProducerSignal(evidence->readyValue, evidence->readyFence->GetCompletedValue());
	}

	RawInputCapture::~RawInputCapture()
	{
		Reset();
		for (auto& slot : slots) {
			if (slot && !GpuFenceRetired(slot->readyFence.get(), slot->readyValue)) {
				std::vector<FenceDependency> dependencies{ FencePoint(slot->readyFence.get(), slot->readyValue) };
				RetireAfterFences(std::move(slot), owner.d3d12Device, std::move(dependencies));
			}
		}
	}

	bool RawInputCapture::Seal()
	{
		if (!active) return false;
		const auto ready = producer.Next();
		if (FAILED(owner.d3d11Context->Signal(producer.fence11.get(), ready))) {
			failure = "Raw input fence signal failed";
			active.reset(); // Slot retains the untracked UINT64_MAX lease.
			return false;
		}
		active->readyValue = ready;
		owner.d3d11Context->Flush();
		return true;
	}

	void RawInputCapture::Reset()
	{
		published.reset();
		if (active) {
			Seal();
			active.reset();
		}
		failedAllocationFrame = UINT32_MAX;
	}

	bool RawInputCapture::Prepare(const GraphicsBackend& backend, RasterIdentityStamp stamp, uint32_t width, uint32_t height)
	{
		if (active && active->stamp == stamp)
			return active->width == width && active->height == height;
		if (active) Reset();
		published.reset();
		if (failedAllocationFrame == stamp.frame) return false;
		if (!backend.d3d11Device || !backend.d3d11Context || !backend.d3d12Device ||
			!width || !height || backend.d3d11Device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1)
			return false;
		if (!producer.fence12) {
			owner = backend;
			producer.Create(owner.d3d12Device.get(), owner.d3d11Device.get(), "NativePBR::InputOnly");
		}
		if (owner.d3d11Device.get() != backend.d3d11Device.get() ||
			owner.d3d12Device.get() != backend.d3d12Device.get() ||
			owner.commandQueue.get() != backend.commandQueue.get() || producer.fence12->GetCompletedValue() == UINT64_MAX)
			return false;
		std::shared_ptr<RawInputFrame>* selected = nullptr;
		for (auto& slot : slots) {
			if (slot && slot->readyValue == UINT64_MAX) {
				const auto rescue = producer.Next();
				if (SUCCEEDED(owner.d3d11Context->Signal(producer.fence11.get(), rescue))) {
					slot->readyValue = rescue;
					owner.d3d11Context->Flush();
				}
			}
			if (!slot || (slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue))) {
				selected = &slot;
				if (slot && slot->width == width && slot->height == height) break;
			}
		}
		if (!selected) { failure = "Raw input frame pool busy"; failedAllocationFrame = stamp.frame; return false; }
		auto& slot = *selected;
		if (!slot || slot->width != width || slot->height != height) {
			const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(kFormat, width, height);
			const auto planeBytes = owner.d3d12Device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
			if (planeBytes > UINT64_MAX / InputOnly::kPlaneCount) return false;
			const auto bytes = planeBytes * InputOnly::kPlaneCount;
			uint64_t tracked = 0;
			for (const auto& held : slots) if (held) tracked += held->allocationBytes;
			const auto limit = budget.Query(owner.d3d12Device.get(), tracked);
			if (!limit.localBudget || bytes == UINT64_MAX || bytes > limit.effectiveBytes ||
				tracked > limit.effectiveBytes - bytes) {
				failure = "Raw input capture deferred by DXGI budget"; failedAllocationFrame = stamp.frame; return false;
			}
			auto replacement = std::make_shared<RawInputFrame>();
			replacement->allocationBytes = bytes;
			replacement->width = width; replacement->height = height;
			D3D11_TEXTURE2D_DESC texture{};
			texture.Width = width; texture.Height = height; texture.MipLevels = 1;
			texture.ArraySize = 1; texture.Format = kFormat;
			texture.SampleDesc.Count = 1; texture.Usage = D3D11_USAGE_DEFAULT;
			texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			// Separate 2D textures match the engine's non-array RTV/DSV dimensions.
			for (UINT slice = 0; slice < InputOnly::kPlaneCount; ++slice) {
				replacement->planes[slice] = std::make_unique<WrappedResource>(texture, owner.d3d11Device.get(),
					owner.d3d12Device.get(), "NativePBR::RawRasterInputs");
			}
			slot = std::move(replacement);
		}
		slot->stamp = stamp; slot->draws = 0; slot->drawRecords.clear(); slot->evidence.reset();
		slot->readyFence = producer.fence12; slot->readyValue = UINT64_MAX;
		active = slot;
		failure.clear();
		return true;
	}

	void RawInputCapture::Publish(const GraphicsBackend& backend, ID3D11Texture2D* mask,
		const std::array<ID3D11Texture2D*, InputOnly::kPlaneCount>& sources,
		RasterIdentityStamp stamp, std::span<const RasterDrawRecord> drawRecords, uint32_t draws)
	{
		published.reset();
		if (!mask || !draws || !backend.d3d11Context) return;
		try {
			D3D11_TEXTURE2D_DESC expected{};
			mask->GetDesc(&expected);
			for (auto* source : sources) {
				if (!source) return;
				D3D11_TEXTURE2D_DESC desc{};
				source->GetDesc(&desc);
				if (desc.Format != kFormat || desc.Width != expected.Width || desc.Height != expected.Height ||
					desc.ArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1) {
					failure = "Native pass MRT extent/format mismatch";
					return;
				}
			}
			if (!Prepare(backend, stamp, expected.Width, expected.Height)) return;
			active->draws = draws;
			active->drawRecords.assign(drawRecords.begin(), drawRecords.end());
			for (uint32_t i = 0; i < InputOnly::kPlaneCount; ++i)
				owner.d3d11Context->CopyResource(active->planes[i]->resource11, sources[i]);
			if (Seal()) {
				active->evidence = evidenceCapture.Capture(owner, mask, stamp);
				if (active->Matches(stamp, active->width, active->height)) published = active;
			}
			active.reset();
		} catch (...) {
			failure = Util::DescribeCurrentD3DException();
			Reset();
		}
	}

	std::shared_ptr<const RawInputFrame> RawInputCapture::GetForFrame(uint32_t frame) const
	{
		return published && published->stamp.frame == frame &&
			published->Matches(published->stamp, published->width, published->height) ? published : nullptr;
	}

	std::shared_ptr<const RawInputFrame> RawInputCapture::Get(RasterIdentityStamp stamp) const
	{
		return published && published->Matches(stamp,published->width,published->height) ? published : nullptr;
	}
}

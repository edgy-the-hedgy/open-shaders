#pragma once

#include "DX12NativeShadowTrace.h"
#include "DX12NativeDirectLighting.h"
#include "GraphicsInterop.h"
#include "VideoMemoryBudget.h"

#include <array>
#include <memory>
#include <vector>

namespace DX12Native
{
	/** @brief Complete, linear, pre-forward opaque color with per-pixel contribution validity. */
	struct OpaqueLightingFrame
	{
		std::shared_ptr<const NativeScreenProbe::MaterialFrame> material;
		std::shared_ptr<const void> allocationOwner;
		winrt::com_ptr<ID3D12Resource> samples;
		winrt::com_ptr<ID3D12Fence> readyFence;
		MaterialFrameContract::TemporalIdentity temporal;
		MaterialFrameContract::LightingCoverage coverage = MaterialFrameContract::LightingCoverage::SelectedPointLights;
		MaterialFrameContract::SignalEncoding encoding = MaterialFrameContract::SignalEncoding::MaterialModulated;
		MaterialFrameContract::SamplingLayout sampling = MaterialFrameContract::SamplingLayout::Sparse;
		uint64_t readyValue = 0;
		uint32_t available = 0, rowPitch = 0;

		bool IsCoherent(const std::shared_ptr<const NativeScreenProbe::MaterialFrame>& expected) const
		{
			using namespace MaterialFrameContract;
			return expected && material == expected && allocationOwner && samples && readyFence &&
				ValidProducerSignal(readyValue, readyFence->GetCompletedValue()) &&
				material->IsCoherent(temporal.nativeFrame, temporal.width, temporal.height) &&
				temporal.SameFrame(material->raster->temporal) &&
				coverage == LightingCoverage::CompleteRasterReplacement &&
				encoding == SignalEncoding::FinalLinearOpaqueColor && sampling == SamplingLayout::FullResolution &&
				ValidLayout(temporal.width, temporal.height, rowPitch, kOpaqueRadianceStride) &&
				samples->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
				samples->GetDesc().Width >= uint64_t(rowPitch) * temporal.height &&
				ResolveOpaqueOwner(SurfaceStatus::EvaluatedBase, material->correspondence,
					{ available, available }, true, true, false) == OpaqueOwner::Native;
		}
	};

	/** @brief Native Composite owns the final pre-upscale DX12 color, as in open-RT. */
	class NativeComposite
	{
	public:
		enum class Transfer
		{
			Unknown,
			Linear,
			LegacyScene
		};

		enum class ColorAuthority { Raster, RasterWithShadowMask, Diagnostic, MixedOpaque };

		struct Frame
		{
			winrt::com_ptr<ID3D12Resource> color;
			winrt::com_ptr<ID3D12Fence> producerFence;
			uint64_t producerValue = 0;
			uint32_t frame = 0, nativeFrame = 0, width = 0, height = 0;
			ColorAuthority colorAuthority = ColorAuthority::Raster;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			bool valid = false;
			bool IsComplete() const
			{
				if (!valid || !producerFence || !producerValue)
					return false;
				const auto completed = producerFence->GetCompletedValue();
				return completed != UINT64_MAX && completed >= producerValue;
			}
		};

		std::shared_ptr<const Frame> Capture(ID3D11Device5* device11,
			ID3D11DeviceContext4* context11, ID3D12Device* device12,
			ID3D12CommandQueue* queue, ID3D11Texture2D* source,
			uint32_t frame, uint32_t nativeFrame, uint32_t width, uint32_t height, Transfer transfer,
			const std::shared_ptr<const ShadowMaskFrame>& shadow,
			bool returnColorToDX11, bool coverageRequested = false,
			const std::shared_ptr<const NativeScreenProbe::MaterialFrame>& material = {},
			const std::shared_ptr<const DirectLightingFrame>& direct = {}, uint32_t lightingView = 0,
			const std::shared_ptr<const OpaqueLightingFrame>& opaque = {});
		bool Retain(const std::shared_ptr<const Frame>& frame, ID3D12Fence* fence, uint64_t value);
		~NativeComposite();

	private:
		struct Slot
		{
			std::shared_ptr<Frame> frame;
			// Retain producer textures until the composite GPU work completes.
			std::shared_ptr<const ShadowMaskFrame> shadowInput;
			std::shared_ptr<const RasterFrame> rasterInput;
			std::shared_ptr<const NativeScreenProbe::MaterialFrame> materialInput;
			std::shared_ptr<const DirectLightingFrame> lightingInput;
			std::shared_ptr<const OpaqueLightingFrame> opaqueInput;
			std::unique_ptr<WrappedResource> shared;
			std::unique_ptr<WrappedResource> returned;
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> commands;
			winrt::com_ptr<ID3D12DescriptorHeap> shadowDescriptors;
			winrt::com_ptr<ID3D12RootSignature> shadowRoot;
			winrt::com_ptr<ID3D12PipelineState> shadowPipeline;
			SharedFence fence;
			std::vector<std::pair<winrt::com_ptr<ID3D12Fence>, uint64_t>> consumers;
			ID3D12Device* device = nullptr;
			ID3D11Device* device11 = nullptr;
			D3D11_TEXTURE2D_DESC sourceDesc{};
			bool supportsShadow = false, hasReturn = false, untracked = false;
			uint64_t frameBytes = 0;
		};

		std::array<std::unique_ptr<Slot>, 4> slots;
		winrt::com_ptr<ID3D12RootSignature> shadowRoot;
		winrt::com_ptr<ID3D12PipelineState> shadowPipeline;
		ID3D12Device* shadowDevice = nullptr;
		VideoMemoryBudget videoBudget;
		bool captureFailureLogged = false;
		bool InitializeShadow(ID3D12Device* device);
	};
}

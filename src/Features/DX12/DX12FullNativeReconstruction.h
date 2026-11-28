#pragma once

#include "DX12NativeDirectLighting.h"
#include "DX12NativeIndirectLighting.h"
#include "DX12FullNativeRasterEvidence.h"

namespace DX12Native
{
	/** @brief Reconstructed contributions and explicit per-pixel opaque ownership bits. */
	struct FullNativeReconstructionFrame
	{
		MaterialFrameContract::ReconstructionState reconstruction = MaterialFrameContract::ReconstructionState::Spatial;
		MaterialFrameContract::StationaryHistoryIdentity historyIdentity{};
		std::shared_ptr<const FullNativeReconstructionFrame> historyInput;
		winrt::com_ptr<ID3D12DescriptorHeap> depthDescriptors;
		static constexpr auto correspondence = MaterialFrameContract::SurfaceCorrespondence::DepthOnly;
		std::shared_ptr<const NativeScreenProbe::MaterialFrame> material;
		std::shared_ptr<const DirectLightingFrame> direct;
		std::shared_ptr<const IndirectLightingFrame> indirect;
		winrt::com_ptr<ID3D12Resource> samples, ownershipMask;
		// Direct diffuse + direct transmission + emission, with explicit validity.
		// The environment producer must never consume combined indirect radiance.
		winrt::com_ptr<ID3D12Resource> environmentRadiance;
		// Authoritative current output, also used as history: six half4 records
		// in order DD, DS, DT, ID, IS, IT. Alpha is the contribution bit;
		// zero alpha means unavailable; positive alpha with zero RGB is valid black.
		// Combined display output is not history.
		winrt::com_ptr<ID3D12Resource> historySignals;
		static constexpr uint32_t historySignalStride = 6 * MaterialFrameContract::kOpaqueRadianceStride;
		static constexpr uint32_t environmentRadianceStride = 8;
		// Session authority is independent of visibility, material and lighting coverage.
		bool nativeSessionAuthority = false;
		// GPU mask ABI v2: bit 0 valid raster visibility, bit 1 optional exact ray
		// match, bit 2 complete lighting (diagnostic), bit 3 native opaque owner,
		// bit 4 evaluated raster material, bit 5 finite evaluated emission.
		// Consumers must not require the diagnostic bits to apply native color.
		static constexpr uint32_t ownershipAbiVersion = 2;
		static constexpr uint32_t evaluatedRasterMaterialBit = 1u << 4;
		static constexpr uint32_t validRasterEmissionBit = 1u << 5;
		bool rasterWinnerCertified = false, completeLightingCoverage = false;
		std::shared_ptr<const NativePBR::RawInputFrame> rasterEvidence;
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList> commands;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12Fence> readyFence;
		uint64_t readyValue = 0, allocationBytes = 0;
		uint32_t frame = 0, width = 0, height = 0, rowPitch = 0, available = 0;
		uint32_t quality = kShadowQualityQuality;
	};

	class FullNativeReconstruction
	{
	public:
		~FullNativeReconstruction();
		void Poll(uint32_t frame);
		void Dispatch(const GraphicsBackend& backend, std::shared_ptr<const NativeScreenProbe::MaterialFrame> material,
			std::shared_ptr<const DirectLightingFrame> direct, std::shared_ptr<const IndirectLightingFrame> indirect,
			uint32_t pluginFrame, bool resetHistory);
		std::shared_ptr<const FullNativeReconstructionFrame> Get(uint32_t frame) const;
	private:
		void Initialize(const GraphicsBackend& backend);
		GraphicsBackend owner;
		VideoMemoryBudget budget;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12Fence> completion;
		std::array<std::shared_ptr<FullNativeReconstructionFrame>, 3> slots;
		std::shared_ptr<FullNativeReconstructionFrame> current;
		std::shared_ptr<const FullNativeReconstructionFrame> history;
		uint64_t nextValue = 0;
		bool failureLogged = false;
	};
}

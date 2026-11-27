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
		// The GPU writes explicit evidence and owner bits. A candidate alone is not authority.
		bool rasterWinnerCertified = false, completeLightingCoverage = false;
		std::shared_ptr<const FullNativeRasterEvidence> rasterEvidence;
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList> commands;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12Fence> readyFence;
		uint64_t readyValue = 0, allocationBytes = 0;
		uint32_t frame = 0, width = 0, height = 0, rowPitch = 0, available = 0;
	};

	class FullNativeReconstruction
	{
	public:
		~FullNativeReconstruction();
		void Poll(uint32_t frame);
		void Dispatch(const GraphicsBackend& backend, std::shared_ptr<const NativeScreenProbe::MaterialFrame> material,
			std::shared_ptr<const DirectLightingFrame> direct, std::shared_ptr<const IndirectLightingFrame> indirect,
			uint32_t pluginFrame, bool resetHistory, ID3D11Texture2D* rasterTokens,
			MaterialFrameContract::RasterIdentityStamp rasterStamp,
			std::span<const MaterialFrameContract::RasterPixelIdentity> tokens);
		std::shared_ptr<const FullNativeReconstructionFrame> Get(uint32_t frame) const;
	private:
		void Initialize(const GraphicsBackend& backend);
		GraphicsBackend owner;
		VideoMemoryBudget budget;
		FullNativeRasterEvidenceCapture evidenceCapture;
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

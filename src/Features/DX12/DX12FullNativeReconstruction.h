#pragma once

#include "DX12NativeDirectLighting.h"
#include "DX12NativeIndirectLighting.h"

namespace DX12Native
{
	/** @brief Independent reconstructed diffuse/specular contribution, never an opaque ownership claim. */
	struct FullNativeReconstructionFrame
	{
		static constexpr auto reconstruction = MaterialFrameContract::ReconstructionState::Spatial;
		static constexpr auto correspondence = MaterialFrameContract::SurfaceCorrespondence::DepthOnly;
		std::shared_ptr<const NativeScreenProbe::MaterialFrame> material;
		std::shared_ptr<const DirectLightingFrame> direct;
		std::shared_ptr<const IndirectLightingFrame> indirect;
		winrt::com_ptr<ID3D12Resource> samples;
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
		void Dispatch(const GraphicsBackend& backend, std::shared_ptr<const NativeScreenProbe::MaterialFrame> material,
			std::shared_ptr<const DirectLightingFrame> direct, std::shared_ptr<const IndirectLightingFrame> indirect);
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
		uint64_t nextValue = 0;
		bool failureLogged = false;
	};
}

#pragma once

#include "DX12FullNativeReconstruction.h"

namespace DX12Native
{
	/** @brief Linear Rec.709 native radiance. Alpha: 0 invalid receiver; 1 valid contribution(s); 2 owned receiver with no available contribution. Both 1 and 2 authorize native color application. */
	struct FullNativeFinalColorFrame
	{
		std::shared_ptr<const FullNativeReconstructionFrame> input;
		std::unique_ptr<WrappedResource> color;
		winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList> commands;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12Fence> readyFence, consumedFence;
		uint64_t readyValue = 0, consumedValue = 0;
		uint32_t frame = 0, width = 0, height = 0;
		bool applied = false;
	};

	class FullNativeFinalColor
	{
	public:
		~FullNativeFinalColor();
		void Poll(uint32_t frame);
		void Compose(const GraphicsBackend& backend, std::shared_ptr<const FullNativeReconstructionFrame> input);
		std::shared_ptr<const FullNativeFinalColorFrame> Get(uint32_t frame) const;
		bool Apply(uint32_t frame, ID3D11UnorderedAccessView* target);
	private:
		void Initialize(const GraphicsBackend& backend);
		GraphicsBackend owner;
		VideoMemoryBudget budget;
		SharedFence producer, consumer;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D11ComputeShader> applyShader;
		std::array<std::shared_ptr<FullNativeFinalColorFrame>, 3> slots;
		std::shared_ptr<FullNativeFinalColorFrame> current;
		bool failureLogged = false;
	};
}

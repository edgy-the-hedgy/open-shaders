#pragma once

#include "GraphicsBackend.h"
#include "RasterCapture.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace DX12Native
{
	/** Optional, fenced SM6.5 diagnostics for copied PBR sidecar inputs. */
	class PBRSidecarValidation
	{
	public:
		using Report = NativePBR::SidecarValidationReport;

		struct Result
		{
			Report report{};
			std::vector<NativePBR::SidecarDraw> draws;
			uint32_t frame = 0, width = 0, height = 0, inspectedX = 0, inspectedY = 0;
			uint32_t missingRecipes = 0, revisionMismatches = 0;
			bool valid = false;
		};

		~PBRSidecarValidation();
		void Advance(const GraphicsBackend& a_backend, std::shared_ptr<const RasterFrame> a_frame);
		void Reset();
		const Result& Latest() const { return latest; }
		const std::string& Status() const { return status; }
		ID3D11ShaderResourceView* Preview() const { return previewSRV.get(); }
		uint32_t PreviewWidth() const { return previewWidth; }
		uint32_t PreviewHeight() const { return previewHeight; }
		bool enabled = false, screenOverlay = false;
		uint32_t mode = 0, intervalFrames = 12, inspectX = 0, inspectY = 0;

	private:
		struct Slot
		{
			std::shared_ptr<const RasterFrame> frame;
			winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
			winrt::com_ptr<ID3D12Resource> report, zero, readback, preview, previewReadback;
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> commands;
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT previewFootprint{};
			uint64_t previewBytes = 0, readyValue = 0;
			uint64_t generation = 0;
			uint32_t previewWidth = 0, previewHeight = 0, inspectedX = 0, inspectedY = 0;
		};

		void CreatePipeline(ID3D12Device* a_device);
		void PrepareSlot(const GraphicsBackend& a_backend, Slot& a_slot, uint32_t a_width, uint32_t a_height);
		void Poll(const GraphicsBackend& a_backend);
		std::array<Slot, 3> slots{};
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12Fence> completion;
		winrt::com_ptr<ID3D12Device> device;
		winrt::com_ptr<ID3D12CommandQueue> queue;
		winrt::com_ptr<ID3D11Texture2D> preview11;
		winrt::com_ptr<ID3D11ShaderResourceView> previewSRV;
		Result latest;
		std::string status;
		uint64_t nextFence = 0;
		uint64_t generation = 1;
		uint32_t lastSubmission = UINT32_MAX, previewWidth = 0, previewHeight = 0;
	};
}

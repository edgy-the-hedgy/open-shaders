#pragma once

#include "DX12MaterialFrameContract.h"
#include "GraphicsBackend.h"
#include "GraphicsInterop.h"
#include "VideoMemoryBudget.h"
#include <array>
#include <memory>
#include <span>

namespace DX12Native
{
	/** @brief MRT token evidence, not a certified raster winner. */
	struct FullNativeRasterEvidence
	{
		// A blended UNORM MRT token is not an independently certified winner.
		static constexpr bool winnerCertified = false;
		MaterialFrameContract::RasterIdentityStamp stamp{};
		std::unique_ptr<WrappedResource> texture;
		winrt::com_ptr<ID3D12Resource> tokenTable;
		winrt::com_ptr<ID3D12Fence> readyFence;
		uint64_t readyValue = 0, allocationBytes = 0;
		uint32_t width = 0, height = 0, tokenCount = 0;
	};

	class FullNativeRasterEvidenceCapture
	{
	public:
		~FullNativeRasterEvidenceCapture();
		std::shared_ptr<const FullNativeRasterEvidence> Capture(const GraphicsBackend& backend,
			ID3D11Texture2D* source, MaterialFrameContract::RasterIdentityStamp stamp,
			std::span<const MaterialFrameContract::RasterPixelIdentity> tokens);
	private:
		GraphicsBackend owner;
		SharedFence producer;
		VideoMemoryBudget budget;
		std::array<std::shared_ptr<FullNativeRasterEvidence>, 3> slots;
		bool failureLogged = false;
	};
}

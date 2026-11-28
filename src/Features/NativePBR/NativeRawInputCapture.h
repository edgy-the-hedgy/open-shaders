#pragma once

#include "Features/DX12/DX12FullNativeRasterEvidence.h"
#include "NativeInputOnlyPolicy.h"
#include <vector>

namespace NativePBR
{
	struct RasterDrawRecord
	{
		uint32_t encodedIdentity = 0, geometryOrdinal = 0, materialOrdinal = 0, sceneInstance = UINT32_MAX;
		uint32_t primitiveCount = 0, primitiveBase = 0, materialIndex = UINT32_MAX, indexCount = 0;
		uint32_t startIndex = 0, indexBufferByteOffset = 0, indexElementBytes = 0;
		int32_t baseVertex = 0;
		uint32_t pixelDescriptor = 0, vertexDescriptor = 0, extraDescriptor = 0, extraFeatureDescriptor = 0;
		uint32_t instanceCount = 1, firstInstance = 0, indexed = 1, reserved = 0;
	};
	static_assert(sizeof(RasterDrawRecord) == 80);
	struct RawInputFrame
	{
		DX12Native::MaterialFrameContract::RasterIdentityStamp stamp{};
		std::array<std::unique_ptr<WrappedResource>, InputOnly::kPlaneCount> planes;
		std::vector<RasterDrawRecord> drawRecords;
		winrt::com_ptr<ID3D12Resource> drawTable;
		std::shared_ptr<const DX12Native::FullNativeRasterEvidence> evidence;
		winrt::com_ptr<ID3D12Fence> readyFence;
		uint64_t readyValue = 0, allocationBytes = 0;
		uint32_t width = 0, height = 0, draws = 0;
		bool Matches(DX12Native::MaterialFrameContract::RasterIdentityStamp requested,
			uint32_t viewWidth, uint32_t viewHeight) const;
	};

	// Original pass owns the integer MRTs. This producer snapshots them once
	// after the engine unbinds the output merger; no per-draw state interception.
	class RawInputCapture
	{
	public:
		~RawInputCapture();
		void Publish(const GraphicsBackend& backend, ID3D11Texture2D* mask,
			const std::array<ID3D11Texture2D*, InputOnly::kPlaneCount>& sources,
			DX12Native::MaterialFrameContract::RasterIdentityStamp stamp,
			std::span<const RasterDrawRecord> drawRecords, uint32_t draws, uint32_t renderWidth, uint32_t renderHeight, bool allWritersKnown);
		std::shared_ptr<const RawInputFrame> GetForFrame(uint32_t frame) const;
		std::shared_ptr<const RawInputFrame> Get(DX12Native::MaterialFrameContract::RasterIdentityStamp stamp) const;
		void Reset();
		const std::string& Failure() const { return failure; }

	private:
		bool Prepare(const GraphicsBackend& backend, DX12Native::MaterialFrameContract::RasterIdentityStamp stamp,
			uint32_t width, uint32_t height);
		bool Seal();
		GraphicsBackend owner;
		SharedFence producer;
		DX12Native::VideoMemoryBudget budget;
		DX12Native::FullNativeRasterEvidenceCapture evidenceCapture;
		std::array<std::shared_ptr<RawInputFrame>, 2> slots;
		std::shared_ptr<RawInputFrame> active, published;
		std::string failure;
		uint32_t failedAllocationFrame = UINT32_MAX;
	};
}

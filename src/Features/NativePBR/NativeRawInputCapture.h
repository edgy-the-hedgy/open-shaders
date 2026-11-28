#pragma once

#include "Features/DX12/DX12FullNativeRasterEvidence.h"
#include "NativeInputOnlyPolicy.h"
#include "Features/DX12/DX12Resources.h"
#include "Features/DX12/DX12TextureRevision.h"
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
	// Source-view identity only, not a copied texture or a residency claim.
	// Slot indices preserve the shader's original resource semantics.
	struct RasterSourceViews
	{
		uint32_t shaderClass = 0;
		// Requested engine sampler address modes for the original low slots.
		std::array<uint8_t, 16> addressModes{};
		// Fur shell color is a material input at t124, not the adjacent depth SRV.
		static constexpr uint32_t kTrackedSlots = 125;
		std::array<winrt::com_ptr<ID3D11ShaderResourceView>, kTrackedSlots> views;
		// Low material slots plus fur t124. CPU-only; no raster ABI change.
		std::array<DX12Native::TextureRevision::ViewStamp,17> revisions{};
		uint64_t revisionEpoch=0,revisionSerial=0;
		DX12Native::TextureRevision::ViewStamp Revision(uint32_t slot) const {
			return slot<16u ? revisions[slot] : slot==124u ? revisions[16] : DX12Native::TextureRevision::ViewStamp{};
		}
	};
	struct RawInputFrame
	{
		DX12Native::MaterialFrameContract::RasterIdentityStamp stamp{};
		std::array<std::unique_ptr<WrappedResource>, InputOnly::kPlaneCount> planes;
		std::vector<RasterDrawRecord> drawRecords;
		// One-based materialOrdinal indexes this CPU metadata snapshot (row ordinal-1).
		std::vector<DX12::PreparedMaterialSource> materialSources;
		// Private producer reuse stamps; consumers retain the same value-row contract.
		std::vector<std::shared_ptr<const DX12::PreparedMaterialSource>> materialSourceSnapshots;
		// Draw ordinal - 1 indexes this optional immutable binding snapshot.
		std::vector<std::shared_ptr<const RasterSourceViews>> drawSourceViews;
		winrt::com_ptr<ID3D12Resource> drawTable;
		std::shared_ptr<const DX12Native::FullNativeRasterEvidence> evidence;
		winrt::com_ptr<ID3D12Fence> readyFence;
		uint64_t readyValue = 0, allocationBytes = 0, drawTableCapacity = 0;
		uint32_t width = 0, height = 0, draws = 0;
		// Primary raster identity is frame/scope local. Geometry and scene material
		// revisions certify only the optional sceneInstance/materialIndex fields.
		bool Matches(DX12Native::MaterialFrameContract::RasterIdentityStamp requested,
			uint32_t viewWidth, uint32_t viewHeight) const;
		bool MatchesRayGeneration(DX12Native::MaterialFrameContract::RasterIdentityStamp requested) const
		{
			return stamp.scope && DX12Native::MaterialFrameContract::MatchesRasterScene(stamp,requested);
		}
		uint32_t ConsumerDrawCount(DX12Native::MaterialFrameContract::RasterIdentityStamp requested) const;
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
			std::span<const RasterDrawRecord> drawRecords, std::span<const std::shared_ptr<const DX12::PreparedMaterialSource>> materials, std::span<const std::shared_ptr<const RasterSourceViews>> sourceViews, uint32_t draws, uint32_t renderWidth, uint32_t renderHeight, bool allWritersKnown);
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

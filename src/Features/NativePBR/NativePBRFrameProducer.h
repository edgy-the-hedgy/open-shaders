#pragma once

#include "Features/DX12/FullNativeFeature.h"

#include "Features/DX12/DX12NativeScreenProbe.h"
#include "NativeRawInputCapture.h"

namespace NativePBR
{
	/** @brief Owns screen-space material evaluation and projected input publications. */
	class FrameProducer final : public DX12::FullNativeFeature
	{
	public:
		std::string GetName() override { return "Native PBR"; }
		std::string GetShortName() override { return "NativePBR"; }
		void OnRuntimeDisabled() override { InvalidateGeneration(); Release(); }
		void ClearShaderCache() override { InvalidateGeneration(); Release(); }
		void Poll(uint32_t frame);
		json GetDiagnostics() override;
		bool WantsFrame(uint32_t frame) const { return !probe || probe->WantsFrame(frame, true); }
		bool HasEvaluator() const { return bool(probe); }
		void Dispatch(const GraphicsBackend& backend, std::shared_ptr<const DX12Native::RasterFrame> raster,
			std::shared_ptr<const DX12Native::TopLevel> scene, uint32_t frame, uint32_t width, uint32_t height, bool opaqueReady, uint32_t quality, std::shared_ptr<const RawInputFrame> rawInputs);
		void PublishProjection(const GraphicsBackend& backend, DX12Native::MaterialFrameContract::RasterIdentityStamp stamp,
			std::span<const DX12Native::ProjectedInputDraw> inputs);
		std::shared_ptr<const DX12Native::ProjectedInputFrame> GetProjection(const DX12Native::MaterialFrameContract::RasterIdentityStamp& stamp) const
		{
			return projection ? projection->Get(stamp) : nullptr;
		}
		std::shared_ptr<const DX12Native::NativeScreenProbe::MaterialFrame> Get(uint32_t frame) const
		{
			return probe ? probe->GetMaterialFrame(frame) : nullptr;
		}
		void InvalidateGeneration();
		void ReleaseProjection() { projection.reset(); }
		void Release();

	private:
		std::unique_ptr<DX12Native::NativeScreenProbe> probe;
		std::unique_ptr<DX12Native::ProjectedInputCapture> projection;
	};
}

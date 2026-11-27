#pragma once

#include "Features/DX12/DX12NativeScreenProbe.h"

namespace NativePBR
{
	/** @brief Owns screen-space material evaluation and projected input publications. */
	class FrameProducer
	{
	public:
		void Poll(uint32_t frame);
		bool WantsFrame(uint32_t frame) const { return !probe || probe->WantsFrame(frame, true); }
		bool HasEvaluator() const { return bool(probe); }
		void Dispatch(const GraphicsBackend& backend, std::shared_ptr<const DX12Native::RasterFrame> raster,
			std::shared_ptr<const DX12Native::TopLevel> scene, uint32_t frame, uint32_t width, uint32_t height, bool opaqueReady);
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
		void Reset();
		void ReleaseProjection() { projection.reset(); }
		void Release();

	private:
		std::unique_ptr<DX12Native::NativeScreenProbe> probe;
		std::unique_ptr<DX12Native::ProjectedInputCapture> projection;
	};
}

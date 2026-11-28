#include "NativePBRFrameProducer.h"

#include "Features/ExtendedMaterials.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

namespace
{
	constexpr uint32_t kDeveloperCoverageInterval = 120;
}

void NativePBR::FrameProducer::Poll(uint32_t frame)
{
	if (probe)
		probe->Poll(frame);
}

void NativePBR::FrameProducer::Dispatch(const GraphicsBackend& backend, std::shared_ptr<const DX12Native::RasterFrame> raster,
	std::shared_ptr<const DX12Native::TopLevel> scene, uint32_t frame, uint32_t width, uint32_t height, bool opaqueReady, uint32_t quality, std::shared_ptr<const RawInputFrame> rawInputs)
{
	if (!scene || !scene->materialEvaluation)
		return;
	if (!probe)
		probe = std::make_unique<DX12Native::NativeScreenProbe>();
	if (!opaqueReady)
		return;
	const DX12Native::MaterialFrameContract::RasterIdentityStamp stamp{ frame, scene->scope, scene->geometryRevision, scene->materialEvaluation->revision };
	auto projected = opaqueReady && projection ? projection->GetSubmitted(stamp) : nullptr;
	const auto& extended = globals::features::extendedMaterials;
	const bool collectCoverage = globals::state && globals::state->IsDeveloperMode() && frame % kDeveloperCoverageInterval == 0;
	probe->Dispatch(backend, std::move(raster), std::move(scene), frame, width, height, true, true, collectCoverage,
		extended.loaded && extended.settings.EnableParallax != 0,
		extended.loaded && extended.settings.EnableParallaxWarpingFix != 0,
		extended.loaded && extended.settings.EnableShadows != 0,
		globals::state->GetMaterialMipBias(Util::GetTemporal()), std::move(projected), std::move(rawInputs), quality);
}

void NativePBR::FrameProducer::PublishProjection(const GraphicsBackend& backend, DX12Native::MaterialFrameContract::RasterIdentityStamp stamp,
	std::span<const DX12Native::ProjectedInputDraw> inputs)
{
	if (!projection)
		projection = std::make_unique<DX12Native::ProjectedInputCapture>();
	projection->Publish(backend, stamp, inputs);
}

void NativePBR::FrameProducer::InvalidateGeneration()
{
	if (projection)
		projection->Reset();
	if (probe)
		probe->Reset();
}

void NativePBR::FrameProducer::Release()
{
	ReleaseProjection();
	probe.reset();
}

json NativePBR::FrameProducer::GetDiagnostics()
{
	const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : UINT32_MAX;
	const auto publication = Get(frame);
	return { { "materialPublished", bool(publication) }, { "materialRevision", publication ? publication->materialRevision : 0 },
		{ "geometryRevision", publication ? publication->geometryRevision : 0 },
		{ "width", publication ? publication->width : 0u }, { "height", publication ? publication->height : 0u },
		{ "producerValue", publication ? publication->producerValue : 0 },
		{ "rawInputDraws", publication ? publication->rawInputDraws : 0u },
		{ "rawInputEvaluatedPixels", probe ? probe->stats.rawInputPixels : 0u },
		{ "primaryRayQueries", probe ? probe->stats.primaryRayQueries : 0u },
		{ "rasterDerivedPixels", probe ? probe->stats.rasterDerivedPixels : 0u },
		{ "unresolvedUntracedPixels", probe ? probe->stats.unresolvedUntracedPixels : 0u },
		{ "submittedFrames", probe ? probe->submittedFrames : 0u }, { "reusedAllocations", probe ? probe->reusedAllocations : 0u },
		{ "coverageMeasured", probe && probe->stats.sampled != 0 },
		{ "coverageFrame", probe ? probe->stats.frame : 0u },
		{ "sampledPixels", probe ? probe->stats.sampled : 0u }, { "evaluatedPixels", probe ? probe->stats.evaluated : 0u },
		{ "baseOnlyPixels", probe ? probe->stats.baseOnly : 0u }, { "depthMismatchPixels", probe ? probe->stats.depthMismatch : 0u },
		{ "missingMaterialPixels", probe ? probe->stats.missingMaterial : 0u }, { "unsupportedMaterialPixels", probe ? probe->stats.unsupportedMaterial : 0u },
		{ "missingTexturePixels", probe ? probe->stats.missingTextures : 0u }, { "failure", probe ? probe->failure : "" } };
}

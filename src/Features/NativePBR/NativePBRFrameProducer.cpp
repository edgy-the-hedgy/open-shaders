#include "NativePBRFrameProducer.h"

#include "Features/ExtendedMaterials.h"
#include "Globals.h"
#include "ShaderCache.h"
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
	if (!rawInputs || !rawInputs->Matches({frame, rawInputs->stamp.scope, 0, 0}, width, height))
		return;
	if (!probe)
		probe = std::make_unique<DX12Native::NativeScreenProbe>();
	if (!opaqueReady)
		return;
	const auto stamp = rawInputs->stamp;
	auto projected = projection ? projection->GetSubmitted(stamp) : nullptr;
	const auto& extended = globals::features::extendedMaterials;
	const bool collectCoverage = globals::state && globals::state->IsDeveloperMode() && frame % kDeveloperCoverageInterval == 0;
	probe->Dispatch(backend, std::move(raster), std::move(scene), frame, width, height, true, true, collectCoverage,
		extended.loaded && extended.settings.EnableParallax != 0,
		extended.loaded && extended.settings.EnableParallaxWarpingFix != 0,
		extended.loaded && extended.settings.EnableShadows != 0,
		globals::state->GetMaterialMipBias(Util::GetTemporal()), std::move(projected), std::move(rawInputs), quality,
		globals::shaderCache ? globals::shaderCache->GetRuntimeTaskGate() : nullptr);
}

bool NativePBR::FrameProducer::PublishProjection(const GraphicsBackend& backend, DX12Native::MaterialFrameContract::RasterIdentityStamp stamp,
	std::span<const DX12Native::ProjectedInputDraw> inputs)
{
	if (!projection)
		projection = std::make_unique<DX12Native::ProjectedInputCapture>();
	projection->Publish(backend, stamp, inputs);
	return bool(projection->GetSubmitted(stamp));
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
	uint32_t missingMaterialDraws=0, typedDraws=0, typedTransferReady=0, typedTransferMissing=0;
	uint32_t terrainInputsRequested=0, terrainInputsReady=0, terrainInputsMissing=0, rasterPatchDraws=0;
	if (publication && publication->rawRasterEvidence) {
		const auto& evidence=*publication->rawRasterEvidence;
		std::vector<uint32_t> typedMasks(evidence.drawRecords.size(),0u);
		if (const auto& transfer=publication->projection) {
			const auto record=[&](uint32_t draw,uint32_t row) {
				if (!draw || draw>typedMasks.size() || row>=transfer->instances.size() ||
					row>=transfer->residentRoleMasks.size()) return;
				const auto kind=transfer->instances[row];
				if ((kind>=DX12Native::kRasterEffectRow && kind<=DX12Native::kRasterGrassRow) || kind==DX12Native::kRasterPbrGrassRow || kind==DX12Native::kRasterTerrainRow)
					typedMasks[draw-1u]=transfer->residentRoleMasks[row]|DX12Native::MaterialFrameContract::kTypedTransferRowPresent;
			};
			for (uint32_t row=0; row<transfer->rasterDraws.size(); ++row)
				record(transfer->rasterDraws[row],row);
			for (const auto& [draw,row]:transfer->rasterAliases) record(draw,row);
		}
		using Type=RE::BSShader::Type;
		for (size_t i=0; i<evidence.drawRecords.size(); ++i) {
			const auto& draw=evidence.drawRecords[i];
			if ((draw.reserved&8u)!=0u) ++rasterPatchDraws;
			const auto* source=i<evidence.drawSourceViews.size() ? evidence.drawSourceViews[i].get() : nullptr;
			if (source) {
				const auto kind=static_cast<Type>(source->shaderClass);
				if (kind==Type::Lighting) {
					using Cache=SIE::ShaderCache;
					using namespace DX12Native::MaterialFrameContract;
					const auto technique=Cache::GetLightingTechnique(draw.pixelDescriptor);
					const bool lod=technique==Cache::LightingShaderTechniques::MTLandLODBlend;
					const bool snow=(draw.pixelDescriptor & uint32_t(Cache::LightingShaderFlags::Snow))!=0u;
					const bool variation=draw.materialOrdinal && draw.materialOrdinal<=evidence.materialSourceSnapshots.size() &&
						evidence.materialSourceSnapshots[draw.materialOrdinal-1u] &&
						(evidence.materialSourceSnapshots[draw.materialOrdinal-1u]->rasterLandscapeVariationFlags&1u)!=0u;
					if (lod || (technique==Cache::LightingShaderTechniques::MTLand && (snow||variation))) {
						++terrainInputsRequested;
						const uint32_t required=kTypedTransferRowPresent|(lod?3u:0u);
						if (HasTypedTransferRoles(typedMasks[i],required)) ++terrainInputsReady;
						else ++terrainInputsMissing;
					}
				}
				if (kind==Type::Grass || kind==Type::DistantTree || kind==Type::Effect) {
					++typedDraws;
					using namespace DX12Native::MaterialFrameContract;
					const uint32_t required=kind==Type::Effect ? TypedEffectRequiredRoles(draw.pixelDescriptor) : kTypedTransferRowPresent|1u;
					// Typed transfer readiness does not imply the authored Grass recipe is available.
					if (kind==Type::Grass && (draw.extraDescriptor&(1u<<13u))!=0u &&
						(!draw.materialOrdinal || draw.materialOrdinal>evidence.materialSourceSnapshots.size() ||
						!evidence.materialSourceSnapshots[draw.materialOrdinal-1u])) ++missingMaterialDraws;
					if (HasTypedTransferRoles(typedMasks[i],required)) ++typedTransferReady;
					else ++typedTransferMissing;
					continue;
				}
			}
			if (!draw.materialOrdinal || draw.materialOrdinal>evidence.materialSourceSnapshots.size() ||
				!evidence.materialSourceSnapshots[draw.materialOrdinal-1u]) ++missingMaterialDraws;
		}
	}
	return { { "materialPublished", bool(publication) },
		{ "rasterMaterialSourceCount", publication ? publication->rasterMaterialSourceCount : 0u },
		{ "rasterUnpreparedSources", publication ? publication->rasterUnpreparedSources : 0u },
		{ "rasterMissingMaterialDraws", missingMaterialDraws },
		{ "typedRasterDraws", typedDraws },
		{ "typedRasterTransferReadyDraws", typedTransferReady },
		{ "typedRasterTransferMissingDraws", typedTransferMissing },
		{ "terrainInputsRequestedDraws", terrainInputsRequested },
		{ "terrainInputsReadyDraws", terrainInputsReady },
		{ "terrainInputsMissingDraws", terrainInputsMissing },
		{ "rasterPatchDraws", rasterPatchDraws },
		{ "secondaryGeometryAvailable", publication && publication->HasSecondaryGeometry() }, { "materialRevision", publication ? publication->materialRevision : 0 },
		{ "geometryRevision", publication ? publication->geometryRevision : 0 },
		{ "rasterMaterialRevision", publication ? publication->rasterMaterialRevision : 0 },
		{ "rasterMaterialBindings", publication && publication->rasterMaterialEvaluation ? publication->rasterMaterialEvaluation->instanceCount : 0u },
		{ "rasterResidentRecipes", publication && publication->rasterMaterialEvaluation ? publication->rasterMaterialEvaluation->residentRecipes : 0u },
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

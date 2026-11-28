#include "NativeIBL.h"

#include "Features/CSUtility.h"
#include "Features/IBL/IBLSettingsInput.h"
#include "Features/NativeCubemaps/NativeCubemapPolicy.h"
#include "Features/DX12/DX12Control.h"
#include "ShaderCache.h"
#include "Util.h"
#include "Features/LinearLighting.h"
#include "Features/Skylighting.h"
#include "Globals.h"
#include "State.h"

#include <algorithm>
#include <cmath>

void NativeIBL::Producer::PostPostLoad()
{
	// Inherit artistic scalar settings, not the legacy feature activation policy.
	// Full Native owns indirect illumination in interiors as well as exteriors.
	settings = globals::features::ibl.settings;
	settings.EnableIBL = 1;
	settings.DisableInInteriors = false;
}

void NativeIBL::Producer::Dispatch(const GraphicsBackend& backend,
	std::shared_ptr<const DX12Native::NativeScreenProbe::MaterialFrame> material,
	std::shared_ptr<const DX12Native::EnvironmentFrame> environment,
	const DX12Native::EnvironmentStamp& stamp,
	std::shared_ptr<const DX12Native::CharacterCasterFrame> characters, uint32_t quality,
	std::shared_ptr<const DX12Native::DirectLightingFrame> ssrDirect)
{
	using namespace DX12Native;
	using namespace DX12Native::MaterialFrameContract;
	if (!loaded || !globals::features::dx12Control.WantsMaterialPreparation() ||
		!material || material->scope != stamp.scope || material->frame != stamp.engineFrame)
		return;
	// Consume Worker 07\'s immutable, completed prior-frame generation only.
	const bool usable = environment && environment->base && environment->base->IsReadyFor(stamp) &&
		NativeCubemaps::Policy::CompletedGenerationFresh(environment->stamp.SameEnvironment(stamp),
			environment->stamp.engineFrame,stamp.engineFrame) && environment->producerFence &&
		environment->producerValue && environment->producerFence->GetCompletedValue()!=UINT64_MAX &&
		environment->producerFence->GetCompletedValue()>=environment->producerValue;
	waitingForEnvironment = !usable;
	if (!usable) {
		// No fabricated publication: the direct/material producers remain independent.
		if (lighting) lighting->Poll(stamp.engineFrame);
		return;
	}
	if (!lighting)
		lighting = std::make_unique<NativeIndirectLighting>();
	const auto color = globals::features::linearLighting.GetCommonBufferData();
	const auto resolved = IBLInput::ResolveSettings(settings);
	const auto utility = globals::features::csUtility.GetCommonBufferData();
	NativeAmbientParameters ambient;
	ambient.dalcValid = globals::state->GetAmbientSH(stamp.engineFrame, stamp.pluginFrame, ambient.dalc);
	ambient.environmentSaturation = resolved.EnvIBLSaturation;
	ambient.skySaturation = resolved.SkyIBLSaturation;
	ambient.dalcMode = resolved.DALCMode;
	ambient.dalcAmount = resolved.DALCAmount;
	ambient.iblEnabled = true;
	// Missing engine ambient cannot prevent valid native cube evaluation.
	if (!ambient.dalcValid) {
		ambient.dalcMode = 0;
		ambient.dalcAmount = 0.0f;
	}
	ambient.ambientMultiplier = utility.ambientLightMult * (color.enableLinearLighting ? color.ambientMult : 1.0f);
	ambient.conversionSaturation = color.conversionSaturation;
	ambient.utilitySaturation = utility.ambientLightColor.saturation;
	ambient.utilityGammaOffset = utility.ambientLightColor.gammaOffset;
	const auto& skylighting = globals::features::skylighting;
	ambient.traceSkyVisibility = false; // Sky occlusion is outside Worker 08 scope.
	ambient.minDiffuseSkyVisibility = std::clamp(skylighting.settings.MinDiffuseVisibility, 0.0f, 1.0f);
	ambient.minSpecularSkyVisibility = std::clamp(skylighting.settings.MinSpecularVisibility, 0.0f, 1.0f);
	ambient.skyMinZ = std::max(0.0f, std::cos(std::clamp(skylighting.settings.MaxZenith, 0.0f, 1.57079632679f)));
	lighting->Dispatch(backend, std::move(material), std::move(environment),
		uint32_t(IndirectDiffuse) | uint32_t(IndirectSpecular) | uint32_t(Transmission),
		resolved.EnvIBLScale, resolved.SkyIBLScale, ambient, std::move(characters), quality, std::move(ssrDirect));
}

json NativeIBL::Producer::GetDiagnostics()
{
	const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : UINT32_MAX;
	const auto publication = Get(frame);
	return { { "indirectPublished", bool(publication) },
		{ "waitingForEnvironment", waitingForEnvironment },
		{ "basePublished", publication && publication->environment && bool(publication->environment->base) },
		{ "reflectionPublished", publication && publication->environment && bool(publication->environment->reflection) },
		{ "baseSH", publication && bool(publication->baseSH) },
		{ "skySH", publication && bool(publication->skySH) },
		{ "environmentContentRevision", publication ? publication->environmentContentRevision : 0u },
        { "skyPublished", publication && bool(publication->environment->sky) },
		{ "availableContributions", publication ? publication->available : 0u },
		{ "environmentRevision", publication && publication->environment ? publication->environment->revision : 0u },
		{ "skyVisibilityTraced", publication && publication->skyVisibilityTraced },
		{ "nativeRayReflectionAdmitted", publication && publication->screenSpaceReflections },
		{ "nativeSSRAdmitted", false }, // Legacy token correspondence is retired.
		{ "allocationBytes", lighting ? lighting->AllocationBytes() : 0 },
		{ "submissions", lighting ? lighting->Submissions() : 0 },
		{ "shProjections", lighting ? lighting->Projections() : 0 },
		{ "failure", lighting ? lighting->Failure() : "" } };
}

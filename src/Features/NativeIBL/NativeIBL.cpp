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
	// Boot settings are inherited, but native evaluation does not depend on
	// legacy IBL dispatch, resource validity or loaded state.
	settings = globals::features::ibl.settings;
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
	// Worker 07 bridge: keep the completed cubemap generation for specular
	// misses. Worker 08 owns diffuse SH/sky visibility and IBL policy.
	const bool usable = environment && environment->base && environment->base->IsReadyFor(stamp) &&
		NativeCubemaps::Policy::CompletedGenerationFresh(environment->stamp.SameEnvironment(stamp),
			environment->stamp.engineFrame,stamp.engineFrame) && environment->producerFence &&
		environment->producerValue && environment->producerFence->GetCompletedValue()!=UINT64_MAX &&
		environment->producerFence->GetCompletedValue()>=environment->producerValue;
	if(!usable) {
		auto ambientEnvironment = std::make_shared<EnvironmentFrame>();
		ambientEnvironment->stamp = stamp;
		ambientEnvironment->revision = stamp.epoch;
		environment = std::move(ambientEnvironment);
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
	ambient.iblEnabled = false; // Worker 08 owns diffuse irradiance integration.
	ambient.ambientMultiplier = utility.ambientLightMult * (color.enableLinearLighting ? color.ambientMult : 1.0f);
	ambient.conversionSaturation = color.conversionSaturation;
	ambient.utilitySaturation = utility.ambientLightColor.saturation;
	ambient.utilityGammaOffset = utility.ambientLightColor.gammaOffset;
	const auto& skylighting = globals::features::skylighting;
	ambient.traceSkyVisibility = false; // RTAO/sky visibility is deferred with cubemap capture.
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
        { "skyPublished", publication && bool(publication->environment->sky) },
		{ "availableContributions", publication ? publication->available : 0u },
		{ "environmentRevision", publication && publication->environment ? publication->environment->revision : 0u },
		{ "skyVisibilityTraced", publication && publication->skyVisibilityTraced },
		{ "nativeSSRAdmitted", publication && publication->screenSpaceReflections },
		{ "allocationBytes", lighting ? lighting->AllocationBytes() : 0 },
		{ "submissions", lighting ? lighting->Submissions() : 0 },
		{ "shProjections", lighting ? lighting->Projections() : 0 },
		{ "failure", lighting ? lighting->Failure() : "" } };
}

#include "NativeIBL.h"

#include "Features/CSUtility.h"
#include "Features/IBL/IBLSettingsInput.h"
#include "Features/DX12/DX12Control.h"
#include "ShaderCache.h"
#include "Util.h"
#include "Features/LinearLighting.h"
#include "Features/Skylighting.h"
#include "Globals.h"
#include "State.h"

#include <algorithm>
#include <cmath>

bool NativeIBL::Producer::HasShaderDefine(RE::BSShader::Type type)
{
	using Type = RE::BSShader::Type;
	return type == Type::Lighting || type == Type::Sky || type == Type::DistantTree || type == Type::Effect;
}

void NativeIBL::Producer::PostPostLoad()
{
	// Boot settings are inherited, but native evaluation does not depend on
	// legacy IBL dispatch, resource validity or loaded state.
	settings = globals::features::ibl.settings;
}

void NativeIBL::Producer::SetupResources()
{
	if (loaded)
		capture.Setup();
}

void NativeIBL::Producer::SetCaptureContext(const DX12Native::EnvironmentStamp& stamp)
{
	if (!context.SameEnvironment(stamp)) {
		capture.Invalidate();
		publishedStamp = {};
	}
	context = stamp;
}

std::function<void()> NativeIBL::Producer::OnReflectionsRenderBegin()
{
	using namespace DX12Native;
	if (!loaded || !globals::features::dx12Control.WantsMaterialPreparation())
		return nullptr;
	const auto resolved = IBLInput::ResolveSettings(settings);
	const bool enabled = resolved.EnableIBL && !Util::IsInterior() &&
		globals::shaderCache && globals::shaderCache->IsEnabled();
	auto cleanup = capture.Begin(enabled);
	if (!cleanup) {
		publishedStamp = {};
		return nullptr;
	}
	auto stamp = context;
	const auto color = globals::features::linearLighting.GetCommonBufferData();
	const auto encoding = color.enableLinearLighting ?
		(color.enableACEScg ? EnvironmentEncoding::LinearAP1 : EnvironmentEncoding::LinearRec709) : EnvironmentEncoding::LegacyScene;
	if (!globals::game::graphicsState || !globals::state || stamp.encoding != encoding || stamp.interior)
		stamp = {};
	else {
		stamp.engineFrame = globals::game::graphicsState->GetFrameCount();
		stamp.pluginFrame = globals::state->frameCount;
	}
	return [this, stamp, cleanup = std::move(cleanup)] {
		cleanup();
		if (!stamp.SameEnvironment(context) || !capture.Ready()) {
			publishedStamp = {};
			return;
		}
		if (capture.ClearedFaces()) {
			publishedStamp = stamp;
			++skyRevision;
		}
	};
}

DX12Native::EnvironmentSource NativeIBL::Producer::GetSkySource(const DX12Native::EnvironmentStamp& requested) const
{
	DX12Native::EnvironmentSource source{ capture.Ready() ? capture.Texture() : nullptr, skyRevision, publishedStamp };
	return source.IsValidFor(requested) ? source : DX12Native::EnvironmentSource{};
}

void NativeIBL::Producer::Dispatch(const GraphicsBackend& backend,
	std::shared_ptr<const DX12Native::NativeScreenProbe::MaterialFrame> material,
	std::shared_ptr<const DX12Native::EnvironmentFrame> environment,
	const DX12Native::EnvironmentStamp& stamp,
	std::shared_ptr<const DX12Native::CharacterCasterFrame> characters)
{
	using namespace DX12Native;
	using namespace DX12Native::MaterialFrameContract;
	if (!material || material->scope != stamp.scope)
		return;
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
	ambient.iblEnabled = resolved.EnableIBL;
	ambient.ambientMultiplier = utility.ambientLightMult * (color.enableLinearLighting ? color.ambientMult : 1.0f);
	ambient.conversionSaturation = color.conversionSaturation;
	ambient.utilitySaturation = utility.ambientLightColor.saturation;
	ambient.utilityGammaOffset = utility.ambientLightColor.gammaOffset;
	const auto& skylighting = globals::features::skylighting;
	ambient.traceSkyVisibility = skylighting.loaded && skylighting.settings.EnableSkylighting && !stamp.interior;
	ambient.minDiffuseSkyVisibility = std::clamp(skylighting.settings.MinDiffuseVisibility, 0.0f, 1.0f);
	ambient.minSpecularSkyVisibility = std::clamp(skylighting.settings.MinSpecularVisibility, 0.0f, 1.0f);
	ambient.skyMinZ = std::max(0.0f, std::cos(std::clamp(skylighting.settings.MaxZenith, 0.0f, 1.57079632679f)));
	lighting->Dispatch(backend, std::move(material), std::move(environment),
		uint32_t(IndirectDiffuse) | uint32_t(IndirectSpecular) | uint32_t(Transmission),
		resolved.EnvIBLScale, resolved.SkyIBLScale, ambient, std::move(characters));
}

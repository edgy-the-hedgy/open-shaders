#include "NativeIBL.h"

#include "Features/CSUtility.h"
#include "Features/IBL.h"
#include "Features/LinearLighting.h"
#include "Features/Skylighting.h"
#include "Globals.h"
#include "State.h"

#include <algorithm>
#include <cmath>

void NativeIBL::Producer::Dispatch(const DX12Native::GraphicsBackend& backend,
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
	const auto& ibl = globals::features::ibl;
	const auto color = globals::features::linearLighting.GetCommonBufferData();
	const auto settings = ibl.GetCommonBufferData();
	const auto utility = globals::features::csUtility.GetCommonBufferData();
	NativeAmbientParameters ambient;
	ambient.dalcValid = globals::state->GetAmbientSH(stamp.engineFrame, stamp.pluginFrame, ambient.dalc);
	ambient.environmentSaturation = settings.EnvIBLSaturation;
	ambient.skySaturation = settings.SkyIBLSaturation;
	ambient.dalcMode = settings.DALCMode;
	ambient.dalcAmount = settings.DALCAmount;
	ambient.iblEnabled = ibl.loaded ? settings.EnableIBL : 0u;
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
		settings.EnvIBLScale, settings.SkyIBLScale, ambient, std::move(characters));
}

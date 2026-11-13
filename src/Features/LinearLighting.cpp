#include "LinearLighting.h"

#include <DirectXTex.h>
#include <cmath>

#include "../I18n/I18n.h"
#include "Features/PostProcessing.h"
#include "GpuPass.h"
#include "ShaderCache.h"
#include "State.h"
<<<<<<< HEAD
#include "TruePBR.h"
#include "Util.h"
||||||| 12b261724
=======
#include "Util.h"
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

<<<<<<< HEAD
#if defined(ENABLE_EFFECTS11)
#	include "Effects11.h"
#	include "Effects11/SettingManager.h"
#endif
||||||| 12b261724
#include "Effects11.h"
#include "Effects11/SettingManager.h"
=======
#include "Effects11.h"
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
#include "Globals.h"
<<<<<<< HEAD
#include "Utils/ColorSpace.h"
||||||| 12b261724
=======
#include "LightLimitFix/Common.h"
#include "PostProcessing.h"
#include "PostProcessingMode.h"
#include "ShaderCache.h"
#include "Utils/ColorSpace.h"
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
#include "Utils/Game.h"
<<<<<<< HEAD
#include "Utils/MathUtils.h"

#include <algorithm>
#include <cmath>

#undef GetObject
||||||| 12b261724
=======
#include "Utils/VersionedRelocation.h"
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

#define I18N_KEY_PREFIX "feature.linear_lighting."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LinearLighting::Settings,
	enableLinearLighting,
<<<<<<< HEAD
	enableACEScg,
	conversionSaturation,
	ambientMult)

namespace
{
	constexpr float kDiffuseGamma = 2.2f;
	constexpr float kDiffuseMidReflectance = 0.5f;
	constexpr float kDiffuseWhiteReflectance = 1.0f;
	constexpr float kDiffuseEncodedMidpoint = 0.5f;
	const float kDiffuseCurve = []() {
		const float midpoint = std::pow(kDiffuseEncodedMidpoint, kDiffuseGamma);
		return kDiffuseMidReflectance * (1.0f - midpoint) / (midpoint * (kDiffuseWhiteReflectance - kDiffuseMidReflectance));
	}();
	constexpr float kSaturationMin = 0.0f;
	constexpr float kSaturationMax = 1.0f;
	constexpr std::array kLinearSRGBLuminanceWeights{ 0.2125f, 0.7154f, 0.0721f };
	constexpr float kSRGBEncodedThreshold = 0.04045f;
	constexpr float kSRGBLinearScale = 12.92f;
	constexpr float kSRGBOffset = 0.055f;
	constexpr float kSRGBScale = 1.055f;
	constexpr float kSRGBExponent = 2.4f;
	constexpr float kMultiplierMin = 0.0f;
	constexpr float kMultiplierMax = 5.0f;

	void SanitizeSettings(LinearLighting::Settings& a_settings)
	{
		const LinearLighting::Settings defaults{};
		a_settings.conversionSaturation = Util::ClampFiniteOrDefault(a_settings.conversionSaturation, kSaturationMin, kSaturationMax, defaults.conversionSaturation);
		a_settings.ambientMult = Util::ClampFinite(a_settings.ambientMult, kMultiplierMin, kMultiplierMax, defaults.ambientMult);
	}
}
||||||| 12b261724
	lightGamma,
	colorGamma,
	emitColorGamma,
	glowmapGamma,
	ambientGamma,
	fogGamma,
	fogAlphaGamma,
	effectGamma,
	effectAlphaGamma,
	skyGamma,
	waterGamma,
	vlGamma,
	vanillaDiffuseColorMult,
	directionalLightMult,
	pointLightMult,
	ambientMult,
	emitColorMult,
	glowmapMult,
	effectLightingMult,
	membraneEffectMult,
	bloodEffectMult,
	projectedEffectMult,
	deferredEffectMult,
	otherEffectMult)
=======
	enableACEScg,
	gameGamma,
	diffuseGamma,
	diffuseMidReflectance,
	diffuseWhiteReflectance,
	directionalLightMult,
	pointLightMult,
	ambientMult,
	emitColorMult,
	glowmapMult,
	effectLightingMult,
	membraneEffectMult,
	bloodEffectMult,
	projectedEffectMult,
	deferredEffectMult,
	otherEffectMult,
	particleEffectMult,
	lightSpriteEffectMult,
	fireEffectMult,
	fireEffectCurve)
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

void LinearLighting::DrawSettings()
{
<<<<<<< HEAD
#if defined(ENABLE_EFFECTS11)
	if (globals::features::effects11.loaded) {
		auto& enb = globals::features::effects11;
		if (enb.enableEffect) {
			ImGui::TextColored(globals::menu->GetSettings().Theme.StatusPalette.Warning, "%s", T("common.settings_managed_by_enb", "Settings are currently managed by ENB."));
			return;
		}
	}
#endif
||||||| 12b261724
	if (globals::features::effects11.loaded) {
		auto& enb = globals::features::effects11;
		if (enb.enableEffect) {
			ImGui::TextColored(globals::menu->GetSettings().Theme.StatusPalette.Warning, "%s", T("common.settings_managed_by_enb", "Settings are currently managed by ENB."));
			return;
		}
	}
=======
	using PostProcessingMode::Mode;
	const Mode mode = PostProcessingMode::Get();
	const bool overridden = mode != Mode::PostProcessing;
	const Util::LockedSection modeLock(overridden, mode == Mode::Effects11 ?
													   T(TKEY("overridden_by_effects11"), "Effects 11 overrides Linear Lighting.") :
													   T(TKEY("off_in_vanilla"), "Vanilla post processing turns Linear Lighting off."));
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

<<<<<<< HEAD
	if (Util::CheckboxFlag(T(TKEY("enable"), "Enable Linear Lighting"), settings.enableLinearLighting))
		weatherLightingColorsInitialized = false;
	Util::CheckboxFlag(T(TKEY("enable_acescg"), "Enable ACEScg Wide Gamut"), settings.enableACEScg);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("enable_acescg_tooltip"),
							  "Render in ACEScg color space for wider gamut and more accurate lighting.\n"
							  "Requires Linear Lighting and Post Processing enabled.\n"
							  "All sRGB-gamut textures and colors will be converted to ACEScg during shading."));
||||||| 12b261724
	ImGui::Checkbox(T(TKEY("enable"), "Enable Linear Lighting"), (bool*)&settings.enableLinearLighting);
=======
	// A disabled checkbox never reports a click, so the saved setting survives the forced-off display.
	bool linearLightingDisplay = settings.enableLinearLighting && !overridden;
	if (ImGui::Checkbox(T(TKEY("enable_linear_lighting"), "Enable Linear Lighting"), &linearLightingDisplay))
		settings.enableLinearLighting = linearLightingDisplay;
	ImGui::Checkbox(T(TKEY("enable_acescg"), "Enable ACEScg Wide Gamut"), (bool*)&settings.enableACEScg);
	Util::Text::Secondary("%s", T(TKEY("startup_settings"), "Linear Lighting and working color space settings require a restart."));
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

<<<<<<< HEAD
	ImGui::SliderFloat(T(TKEY("vanilla_diffuse_saturation"), "Conversion Saturation"), &settings.conversionSaturation, kSaturationMin, kSaturationMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("ambient_multiplier"), "Ambient Multiplier"), &settings.ambientMult, kMultiplierMin, kMultiplierMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
||||||| 12b261724
	if (ImGui::BeginTabBar("##LinearLightingTabs", ImGuiTabBarFlags_None)) {
		if (ImGui::BeginTabItem(T(TKEY("tab_general"), "General"))) {
			ImGui::SeparatorText(T(TKEY("gamma_settings"), "Gamma Settings"));
			ImGui::SliderFloat(T(TKEY("fog_gamma"), "Fog Gamma"), &settings.fogGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("fog_transparency_gamma"), "Fog Transparency Gamma"), &settings.fogAlphaGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("sky_gamma"), "Sky Gamma"), &settings.skyGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("vl_gamma"), "Volumetric Lighting Gamma"), &settings.vlGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("water_gamma"), "Water Gamma"), &settings.waterGamma, 0.1f, 3.0f, "%.2f");

			ImGui::SeparatorText(T(TKEY("multipliers"), "Multipliers"));
			ImGui::SliderFloat(T(TKEY("directional_light_multiplier"), "Directional Light Multiplier"), &settings.directionalLightMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("ambient_multiplier"), "Ambient Multiplier"), &settings.ambientMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("glowmap_multiplier"), "Glowmap Multiplier"), &settings.glowmapMult, 0.0f, 10.0f, "%.2f");

			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(T(TKEY("tab_advanced"), "Advanced"))) {
			ImGui::SeparatorText(T(TKEY("gamma_settings"), "Gamma Settings"));
			ImGui::SliderFloat(T(TKEY("light_gamma"), "Light Gamma"), &settings.lightGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("color_gamma"), "Color Gamma"), &settings.colorGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("emissive_color_gamma"), "Emissive Color Gamma"), &settings.emitColorGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("glowmap_gamma"), "Glowmap Gamma"), &settings.glowmapGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("ambient_gamma"), "Ambient Gamma"), &settings.ambientGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("effect_gamma"), "Effect Gamma"), &settings.effectGamma, 0.1f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("effect_transparency_gamma"), "Effect Transparency Gamma"), &settings.effectAlphaGamma, 0.1f, 3.0f, "%.2f");

			ImGui::SeparatorText(T(TKEY("multipliers"), "Multipliers"));
			ImGui::SliderFloat(T(TKEY("vanilla_diffuse_color_multiplier"), "Vanilla Diffuse Color Multiplier"), &settings.vanillaDiffuseColorMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("emissive_color_multiplier"), "Emissive Color Multiplier"), &settings.emitColorMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("point_light_multiplier"), "Point Light Multiplier"), &settings.pointLightMult, 0.0f, 10.0f, "%.2f");

			if (ImGui::TreeNodeEx(T(TKEY("effects"), "Effects"), ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::SliderFloat(T(TKEY("effect_lighting_multiplier"), "Effect Lighting Multiplier"), &settings.effectLightingMult, 0.0f, 10.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("membrane_effects_multiplier"), "Membrane Effects Multiplier"), &settings.membraneEffectMult, 0.0f, 10.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("blood_effects_multiplier"), "Blood Effects Multiplier"), &settings.bloodEffectMult, 0.0f, 10.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("projected_effects_multiplier"), "Projected Effects Multiplier"), &settings.projectedEffectMult, 0.0f, 10.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("deferred_effects_multiplier"), "Deferred Effects Multiplier"), &settings.deferredEffectMult, 0.0f, 10.0f, "%.2f");
				ImGui::SliderFloat(T(TKEY("other_effects_multiplier"), "Other Effects Multiplier"), &settings.otherEffectMult, 0.0f, 10.0f, "%.2f");
				ImGui::TreePop();
			}

			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}
=======
	if (Util::BeginPillTabBar("##LinearLightingTabs")) {
		if (Util::BeginPillTabItem(T(TKEY("tab_general"), "General"))) {
			ImGui::SliderFloat(T(TKEY("directional_light_multiplier"), "Directional Light Multiplier"), &settings.directionalLightMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("ambient_multiplier"), "Ambient Multiplier"), &settings.ambientMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("glowmap_multiplier"), "Glowmap Multiplier"), &settings.glowmapMult, 0.0f, 10.0f, "%.2f");

			Util::EndPillTabItem();
		}

		if (Util::BeginPillTabItem(T(TKEY("tab_advanced"), "Advanced"))) {
			ImGui::SeparatorText(T(TKEY("color_calibration"), "Color Calibration"));
			ImGui::InputFloat(T(TKEY("diffuse_gamma"), "Diffuse Gamma"), &settings.diffuseGamma, 0.0f, 0.0f, "%.3f");
			ImGui::InputFloat(T(TKEY("diffuse_mid_reflectance"), "Diffuse Reflectance at 0.5"), &settings.diffuseMidReflectance, 0.0f, 0.0f, "%.6f");
			ImGui::InputFloat(T(TKEY("diffuse_white_reflectance"), "Diffuse White Reflectance"), &settings.diffuseWhiteReflectance, 0.0f, 0.0f, "%.3f");
			ImGui::TextWrapped("%s", T(TKEY("color_calibration_hint"), "Diffuse settings update immediately. Gamma: 0.1 to 4. White: 0.001 to 1. Midpoint: 0.0001 to 99.9% of white."));
			ImGui::InputFloat(T(TKEY("game_gamma"), "Game Gamma"), &settings.gameGamma, 0.0f, 0.0f, "%.3f");
			const bool valid = std::isfinite(settings.gameGamma) && settings.gameGamma >= 0.1f && settings.gameGamma <= 4.0f;
			ImGui::TextWrapped("%s", T(TKEY("game_gamma_hint"), "Game Gamma: 0.1 to 4. Apply to recompile shaders."));
			ImGui::BeginDisabled(!valid);
			if (Util::Button(T(TKEY("apply_game_gamma"), "Apply Game Gamma")))
				applyGameGamma = settings.gameGamma != configuredGameGamma;
			ImGui::EndDisabled();
			ImGui::SeparatorText(T(TKEY("multipliers"), "Multipliers"));
			ImGui::SliderFloat(T(TKEY("emissive_color_multiplier"), "Emissive Color Multiplier"), &settings.emitColorMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("point_light_multiplier"), "Point Light Multiplier"), &settings.pointLightMult, 0.0f, 10.0f, "%.2f");

			ImGui::SeparatorText(T(TKEY("effects"), "Effects"));
			ImGui::SliderFloat(T(TKEY("effect_lighting_multiplier"), "Effect Lighting Multiplier"), &settings.effectLightingMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("membrane_effects_multiplier"), "Membrane Effects Multiplier"), &settings.membraneEffectMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("blood_effects_multiplier"), "Blood Effects Multiplier"), &settings.bloodEffectMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("projected_effects_multiplier"), "Projected Effects Multiplier"), &settings.projectedEffectMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("deferred_effects_multiplier"), "Deferred Effects Multiplier"), &settings.deferredEffectMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("other_effects_multiplier"), "Other Effects Multiplier"), &settings.otherEffectMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("particle_effects_multiplier"), "Particle Effects Multiplier"), &settings.particleEffectMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("light_sprite_effects_multiplier"), "Light Sprite Effects Multiplier"), &settings.lightSpriteEffectMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("fire_effects_multiplier"), "Fire Effects Multiplier"), &settings.fireEffectMult, 0.0f, 10.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("fire_effects_curve"), "Fire Effects Curve"), &settings.fireEffectCurve, FireEffectCurveMin, FireEffectCurveMax, "%.2f");

			Util::EndPillTabItem();
		}

		Util::EndPillTabBar();
	}
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
}

void LinearLighting::LoadSettings(json& o_json)
{
	settings = o_json;
<<<<<<< HEAD
	if (!o_json.contains("conversionSaturation")) {
		if (const auto legacy = o_json.find("vanillaDiffuseSaturation"); legacy != o_json.end() && legacy->is_number())
			settings.conversionSaturation = legacy->get<float>();
	}
	SanitizeSettings(settings);
	weatherLightingColorsInitialized = false;
||||||| 12b261724
=======
	if (o_json.contains("mode") && !o_json.contains("enableLinearLighting"))
		settings.enableLinearLighting = o_json.value("mode", 0u) == 1u;
	if (o_json.contains("vanillaDiffuseColorMult") && !o_json.contains("diffuseMidReflectance")) {
		const float scale = o_json.value("vanillaDiffuseColorMult", 1.0f);
		settings.diffuseMidReflectance *= scale;
		if (!o_json.contains("diffuseWhiteReflectance"))
			settings.diffuseWhiteReflectance = std::min(scale, 1.0f);
	}
	settings.fireEffectCurve = std::clamp(settings.fireEffectCurve, FireEffectCurveMin, FireEffectCurveMax);
	applyGameGamma = settings.gameGamma != configuredGameGamma;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
}

void LinearLighting::SaveSettings(json& o_json)
{
	SanitizeSettings(settings);
	o_json = settings;
}

bool LinearLighting::IsLinearLightingActive() const
{
	return configuredLinearLighting && !globals::features::effects11.IsActive();
}

void LinearLighting::PostSetupResources()
{
	assert(!configuredLinearLighting || globals::features::postProcessing.loaded);
	resourcesReady = true;
}

void LinearLighting::Reset()
{
	if (!resourcesReady)
		return;
	if (applyGameGamma && !IsLinearLightingActive())
		ApplyGameGamma();
	// Needs Post Processing's display transform, so Vanilla mode turns it off; Effects 11 gates it separately via IsActive.
	const bool linearLighting = settings.enableLinearLighting && globals::features::postProcessing.loaded &&
	                            PostProcessingMode::Get() != PostProcessingMode::Mode::Vanilla;
	const bool acescg = linearLighting && settings.enableACEScg;
	if (linearLighting != configuredLinearLighting || acescg != configuredACEScg || applyGameGamma)
		globals::shaderCache->Reload([this, linearLighting, acescg] {
			configuredLinearLighting = linearLighting;
			configuredACEScg = acescg;
			if (applyGameGamma)
				ApplyGameGamma();
		});
}

void LinearLighting::ApplyGameGamma()
{
	settings.gameGamma = std::clamp(std::isfinite(settings.gameGamma) ? settings.gameGamma : 1.6f, 0.1f, 4.0f);
	configuredGameGamma = settings.gameGamma;
	gameGammaDefine = std::format("{:.9f}", settings.gameGamma);
	applyGameGamma = false;
}

void LinearLighting::ClearShaderCache()
{
	if (!globals::d3d::context || !globals::game::renderer)
		return;
	globals::state->RequestHistoryReset();
	workingSunlight = nullptr;
	const float clear[4]{};
	auto& reflections = globals::game::renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];
	for (auto* rtv : reflections.cubeSideRTV)
		if (rtv)
			globals::d3d::context->ClearRenderTargetView(rtv, clear);
}

std::vector<std::pair<std::string_view, std::string_view>> LinearLighting::GetCommonShaderDefines()
{
	auto defines = GetShaderDefineOptions();
	if (IsLinearLightingActive())
		defines.emplace_back("ENABLE_LL", "");
	return defines;
}

std::vector<std::pair<std::string_view, std::string_view>> LinearLighting::GetShaderDefineOptions()
{
	std::vector<std::pair<std::string_view, std::string_view>> options;
	if (IsACEScgActive())
		options.emplace_back("ENABLE_ACESCG", "");
	if (IsLinearLightingActive())
		options.emplace_back("CS_GAME_GAMMA", gameGammaDefine);
	return options;
}

void LinearLighting::RestoreDefaultSettings()
{
	settings = {};
<<<<<<< HEAD
	weatherLightingColorsInitialized = false;
}

void LinearLighting::SetupResources()
{
	PerGeometryCB = new ConstantBuffer(ConstantBufferDesc<PerGeometryData>(), "LinearLighting::PerGeometryCB");
	CompileSceneGammaDecodeShader();
	sceneGammaActive = false;
	weatherLightingColorsInitialized = false;
}

void LinearLighting::ClearShaderCache()
{
	CompileSceneGammaDecodeShader();
}

void LinearLighting::CompileSceneGammaDecodeShader()
{
	sceneGammaDecodeCS = nullptr;
	if (!globals::game::renderer) {
		logger::warn("[LinearLighting] Renderer unavailable; scene gamma decode disabled");
		return;
	}

	auto& mainTarget = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMAIN];
	if (!mainTarget.UAV) {
		logger::warn("[LinearLighting] kMAIN has no UAV; scene gamma decode disabled");
		return;
	}

	D3D11_UNORDERED_ACCESS_VIEW_DESC mainUAVDesc{};
	Util::AsReal(mainTarget.UAV)->GetDesc(&mainUAVDesc);
	if (!State::SupportsTypedUAVLoad(mainUAVDesc.Format)) {
		logger::warn("[LinearLighting] kMAIN format lacks typed UAV load support; scene gamma decode disabled");
		return;
	}

	sceneGammaDecodeCS.attach(static_cast<ID3D11ComputeShader*>(
		Util::CompileShader(L"Data\\Shaders\\LinearLighting\\SceneGammaDecodeCS.hlsl", {}, "cs_5_0")));
}

void LinearLighting::OnWorldRenderBegin()
{
	sceneGammaActive = false;
	globals::state->permutationData.ExtraShaderDescriptor &= ~static_cast<uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);

	if (!globals::state->inWorld || !IsLinearLightingActive())
		return;

	sceneGammaActive = true;
	globals::state->permutationData.ExtraShaderDescriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);
}

void LinearLighting::OnWorldRenderEnd(RE::RENDER_TARGET a_renderTarget)
{
	if (!globals::state) {
		sceneGammaActive = false;
		return;
	}
	constexpr auto gammaRenderTarget = static_cast<uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);
	if (!sceneGammaActive && (globals::state->permutationData.ExtraShaderDescriptor & gammaRenderTarget) == 0)
		return;
	const SKSE::stl::scope_exit completeSceneGamma([this]() noexcept {
		sceneGammaActive = false;
		globals::state->permutationData.ExtraShaderDescriptor &= ~static_cast<uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);
	});
	if (!sceneGammaActive || (globals::state->permutationData.ExtraShaderDescriptor & gammaRenderTarget) == 0)
		return;

	const auto targetIndex = static_cast<size_t>(a_renderTarget);
	if (!sceneGammaDecodeCS || !globals::game::renderer || targetIndex >= static_cast<size_t>(Util::GetRenderTargetCount()))
		return;

	auto& target = globals::game::renderer->GetRuntimeData().renderTargets[targetIndex];
	auto* targetUAV = Util::AsReal(target.UAV);
	if (!targetUAV)
		return;
	if (!globals::d3d::context || !globals::state->sharedDataCB || !globals::state->featureDataCB)
		return;

	uint32_t width = 0;
	uint32_t height = 0;
	if (a_renderTarget == RE::RENDER_TARGET::kMAIN) {
		const auto screenSize = Util::ConvertToDynamic(globals::state->screenSize, true);
		if (!std::isfinite(screenSize.x) || !std::isfinite(screenSize.y) || screenSize.x <= 0.0f || screenSize.y <= 0.0f)
			return;
		width = static_cast<uint32_t>(std::ceil(screenSize.x));
		height = static_cast<uint32_t>(std::ceil(screenSize.y));
	} else {
		if (!target.texture)
			return;
		D3D11_TEXTURE2D_DESC textureDesc{};
		Util::AsReal(target.texture)->GetDesc(&textureDesc);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		targetUAV->GetDesc(&uavDesc);
		if (uavDesc.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2D)
			return;
		const auto mipLevel = uavDesc.Texture2D.MipSlice;
		if (mipLevel >= 32 || (textureDesc.MipLevels != 0 && mipLevel >= textureDesc.MipLevels))
			return;
		width = std::max(textureDesc.Width >> mipLevel, 1u);
		height = std::max(textureDesc.Height >> mipLevel, 1u);
	}
	if (width == 0 || height == 0)
		return;

	CS_GPU_PASS("LinearLighting::DecodeScene");
	auto* context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->CSSetShader(sceneGammaDecodeCS.get(), nullptr, 0);
	globals::state->BindSharedDataCS(context);
	context->CSSetUnorderedAccessViews(0, 1, &targetUAV, nullptr);

	context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAV = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void LinearLighting::OnBeforePostProcessing(RE::RENDER_TARGET a_renderTarget)
{
	if (a_renderTarget == RE::RENDER_TARGET::kMAIN)
		OnWorldRenderEnd(a_renderTarget);
}

std::function<void()> LinearLighting::OnReflectionsRenderBegin()
{
	if (!IsLinearLightingActive() || !globals::game::renderer)
		return nullptr;

	constexpr auto gammaRenderTarget = static_cast<uint32_t>(State::ExtraShaderDescriptors::GammaRenderTarget);
	auto* const state = globals::state;
	const auto previousGammaRenderTarget = state->permutationData.ExtraShaderDescriptor & gammaRenderTarget;
	auto& clearColor = globals::game::renderer->GetRendererData().clearColor;
	const auto previousClearColor = std::to_array(clearColor);
	const auto linearLightingData = GetCommonBufferData();
	const auto linearColor = DecodeAuthoredColor({ clearColor[0], clearColor[1], clearColor[2] }, linearLightingData.conversionSaturation);
	DirectX::SimpleMath::Vector3 reflectionClearColor{ linearColor.red, linearColor.green, linearColor.blue };
	if (linearLightingData.enableACEScg) {
		static const auto srgbToACEScg = getWhiteAdaptedRGBMatrix("sRGB", "ACEScg").Transpose();
		reflectionClearColor = DirectX::SimpleMath::Vector3::Transform(reflectionClearColor, srgbToACEScg);
	}
	clearColor[0] = reflectionClearColor.x;
	clearColor[1] = reflectionClearColor.y;
	clearColor[2] = reflectionClearColor.z;

	state->permutationData.ExtraShaderDescriptor &= ~gammaRenderTarget;
	return [state, &clearColor, previousClearColor, previousGammaRenderTarget]() {
		std::ranges::copy(previousClearColor, clearColor);
		state->permutationData.ExtraShaderDescriptor = (state->permutationData.ExtraShaderDescriptor & ~gammaRenderTarget) | previousGammaRenderTarget;
	};
}

void LinearLighting::Prepass()
{
	dirLightMult = 1.0f;
	if (!IsLinearLightingActive())
		return;

	lodProjectedMaterialColorScales.fill(kNoProjectedMaterialColorScale);
	if (auto* defaultObjects = RE::BGSDefaultObjectManager::GetSingleton(); defaultObjects && globals::features::truePBR.loaded) {
		constexpr std::array materialObjects{ RE::DefaultObjectID::kSnowLODMaterial, RE::DefaultObjectID::kSnowLODMaterialHD };
		for (size_t index = 0; index < materialObjects.size(); ++index) {
			auto** materialObject = defaultObjects->GetObject<RE::BGSMaterialObject>(materialObjects[index]);
			auto* materialData = globals::features::truePBR.GetPBRMaterialObjectData(materialObject ? *materialObject : nullptr);
			if (materialData && std::ranges::all_of(materialData->baseColorScale, [](float value) { return std::isfinite(value); })) {
				std::ranges::transform(materialData->baseColorScale, lodProjectedMaterialColorScales[index].begin(), [](float value) { return std::max(value, 0.0f); });
			}
		}
	}

	auto imageSpaceManager = globals::game::imageSpaceManager;
	if (!imageSpaceManager)
		return;

	dirLightMult = imageSpaceManager->GetImageSpaceData().baseData.hdr.sunlightScale;
}

struct LinearLighting::Hooks
{
	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
		{
			globals::features::linearLighting.BSLightingShader_SetupGeometry(Pass);
			func(This, Pass, RenderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void Install()
	{
		stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
		logger::info("[LinearLighting] Installed lighting shader hook");
	}
};

void LinearLighting::PostPostLoad()
{
	LinearLighting::Hooks::Install();
||||||| 12b261724
}

void LinearLighting::SetupResources()
{
	PerGeometryCB = new ConstantBuffer(ConstantBufferDesc<PerGeometryData>());
}

void LinearLighting::Prepass()
{
	bool menuArtOpen = globals::state->IsMainLoadingOrFlatMapOpen();
	dirLightMult = 1.0f;
	if (!settings.enableLinearLighting || menuArtOpen)
		return;

	auto imageSpaceManager = globals::game::imageSpaceManager;
	if (!imageSpaceManager)
		return;

	dirLightMult = imageSpaceManager->GetRuntimeData().data.baseData.hdr.sunlightScale;
}

struct LinearLighting::Hooks
{
	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
		{
			globals::features::linearLighting.BSLightingShader_SetupGeometry(Pass);
			func(This, Pass, RenderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void Install()
	{
		stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
		logger::info("[LinearLighting] Installed hooks - BSLightingShader_SetupGeometry");
	}
};

void LinearLighting::PostPostLoad()
{
	LinearLighting::Hooks::Install();
=======
	applyGameGamma = settings.gameGamma != configuredGameGamma;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
}

LinearLighting::PerFrameData LinearLighting::GetCommonBufferData()
{
<<<<<<< HEAD
	if (!loaded) {
		auto data = PerFrameData{};
		data.enableLinearLighting = false;
		return data;
	}
||||||| 12b261724
	if (!loaded) {
		auto data = PerFrameData{};
		data.enableLinearLighting = false;
		return data;
	}
	bool menuArtOpen = globals::state->IsMainLoadingOrFlatMapOpen();
=======
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	auto data = PerFrameData{};
<<<<<<< HEAD
	data.enableLinearLighting = IsLinearLightingActive();
	data.enableACEScg = settings.enableACEScg && data.enableLinearLighting && globals::features::postProcessing.loaded;
	data.isDirLightLinear = isDirLightLinear;
	data.dirLightMult = dirLightMult;
||||||| 12b261724
	data.enableLinearLighting = settings.enableLinearLighting && !menuArtOpen;
	data.isDirLightLinear = isDirLightLinear;
	data.dirLightMult = dirLightMult;
	data.lightGamma = settings.lightGamma;
	data.colorGamma = settings.colorGamma;
	data.emitColorGamma = settings.emitColorGamma;
	data.glowmapGamma = settings.glowmapGamma;
	data.ambientGamma = settings.ambientGamma;
	data.fogGamma = settings.fogGamma;
	data.fogAlphaGamma = settings.fogAlphaGamma;
	data.effectGamma = settings.effectGamma;
	data.effectAlphaGamma = settings.effectAlphaGamma;
	data.skyGamma = settings.skyGamma;
	data.waterGamma = settings.waterGamma;
	data.vlGamma = settings.vlGamma;
=======
	const auto bounded = [](float value, float fallback, float min, float max) {
		return std::clamp(std::isfinite(value) ? value : fallback, min, max);
	};
	data.diffuseGamma = bounded(settings.diffuseGamma, 2.2f, 0.1f, 4.0f);
	data.diffuseWhiteReflectance = bounded(settings.diffuseWhiteReflectance, 1.0f, 0.001f, 1.0f);
	const float reflectance = bounded(settings.diffuseMidReflectance, 0.2871746f, 0.0001f, data.diffuseWhiteReflectance * 0.999f);
	const float midpoint = std::pow(0.5f, data.diffuseGamma);
	data.diffuseCurve = reflectance * (1.0f - midpoint) / (midpoint * (data.diffuseWhiteReflectance - reflectance));
	data.directionalLightMult = 1.0f;
	data.pointLightMult = 1.0f;
	data.ambientMult = 1.0f;
	data.glowmapMult = 1.0f;
	data.effectLightingMult = 1.0f;
	data.membraneEffectMult = 1.0f;
	data.bloodEffectMult = 1.0f;
	data.projectedEffectMult = 1.0f;
	data.deferredEffectMult = 1.0f;
	data.otherEffectMult = 1.0f;
	data.particleEffectMult = 1.0f;
	data.lightSpriteEffectMult = 1.0f;
	data.fireEffectMult = 1.0f;
	data.fireEffectCurve = 1.0f;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

<<<<<<< HEAD
	Settings sanitizedSettings = settings;
	SanitizeSettings(sanitizedSettings);
	data.diffuseGamma = kDiffuseGamma;
	data.diffuseWhiteReflectance = kDiffuseWhiteReflectance;
	data.conversionSaturation = sanitizedSettings.conversionSaturation;
	data.diffuseCurve = kDiffuseCurve;
	data.ambientMult = sanitizedSettings.ambientMult;
	if (data.enableLinearLighting && (!weatherLightingColorsInitialized || data.conversionSaturation != weatherConversionSaturation))
		OnWeatherColorsUpdated(globals::game::sky);
	data.effectLightingColor = effectLightingColor;
	data.skyStaticsColor = skyStaticsColor;

	// Override multipliers to neutral values when ENB PP is active
#if defined(ENABLE_EFFECTS11)
	if (globals::features::effects11.loaded) {
		auto& enb = globals::features::effects11;
		if (enb.enableEffect) {
			data.ambientMult = 1.0f;
			data.dirLightMult = 1.0f;
		}
	}
#endif
||||||| 12b261724
	if (globals::features::effects11.loaded) {
		auto& enb = globals::features::effects11;
		if (enb.enableEffect) {
			data.enableLinearLighting = false;
			data.lightGamma = 1.0f;
			data.colorGamma = 1.0f;
			data.emitColorGamma = 1.0f;
			data.glowmapGamma = 1.0f;
			data.ambientGamma = 1.0f;
			data.fogGamma = 1.0f;
			data.fogAlphaGamma = 1.0f;
			data.effectGamma = 1.0f;
			data.effectAlphaGamma = 1.0f;
			data.skyGamma = 1.0f;
			data.waterGamma = 1.0f;
			data.vlGamma = 1.0f;
		}
	}

	data.vanillaDiffuseColorMult = settings.vanillaDiffuseColorMult;
	data.directionalLightMult = settings.directionalLightMult;
	data.pointLightMult = settings.pointLightMult;
	data.ambientMult = settings.ambientMult;
	data.emitColorMult = settings.emitColorMult;
	data.glowmapMult = settings.glowmapMult;
	data.effectLightingMult = settings.effectLightingMult;
	data.membraneEffectMult = settings.membraneEffectMult;
	data.bloodEffectMult = settings.bloodEffectMult;
	data.projectedEffectMult = settings.projectedEffectMult;
	data.deferredEffectMult = settings.deferredEffectMult;
	data.otherEffectMult = settings.otherEffectMult;

	// Override multipliers to neutral values when ENB PP is active
	if (globals::features::effects11.loaded) {
		auto& enb = globals::features::effects11;
		if (enb.enableEffect) {
			data.vanillaDiffuseColorMult = 1.0f;
			data.directionalLightMult = 1.0f;
			data.pointLightMult = 1.0f;
			data.ambientMult = 1.0f;
			data.emitColorMult = 1.0f;
			data.glowmapMult = 1.0f;
			data.effectLightingMult = 1.0f;
			data.membraneEffectMult = 1.0f;
			data.bloodEffectMult = 1.0f;
			data.projectedEffectMult = 1.0f;
			data.deferredEffectMult = 1.0f;
			data.otherEffectMult = 1.0f;
		}
	}
=======
	data.isMainOrLoadingMenu = globals::state->IsMainOrLoadingMenuOpen();

	if (!loaded || !IsLinearLightingActive())
		return data;

	// Legacy light colors represent the response of a white Lambert surface.
	data.directionalLightMult = RE::NI_PI * settings.directionalLightMult;
	data.pointLightMult = RE::NI_PI * settings.pointLightMult;
	data.ambientMult = settings.ambientMult;
	data.glowmapMult = settings.glowmapMult;
	data.effectLightingMult = settings.effectLightingMult;
	data.membraneEffectMult = settings.membraneEffectMult;
	data.bloodEffectMult = settings.bloodEffectMult;
	data.projectedEffectMult = settings.projectedEffectMult;
	data.deferredEffectMult = settings.deferredEffectMult;
	data.otherEffectMult = settings.otherEffectMult;
	data.particleEffectMult = settings.particleEffectMult;
	data.lightSpriteEffectMult = settings.lightSpriteEffectMult;
	data.fireEffectMult = settings.fireEffectMult;
	data.fireEffectCurve = settings.fireEffectCurve;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	return data;
}

<<<<<<< HEAD
bool LinearLighting::IsLinearLightingActive() const
||||||| 12b261724
RE::NiColor LinearLighting::ColorToLinear(RE::NiColor inColor, float gamma)
=======
void LinearLighting::ModifySharedLighting(SharedLighting& lighting)
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
{
<<<<<<< HEAD
	if (!loaded || !settings.enableLinearLighting || !sceneGammaDecodeCS || !globals::state || !globals::shaderCache ||
		!globals::shaderCache->IsEnabled() || globals::state->IsMainLoadingOrFlatMapOpen())
		return false;

#if defined(ENABLE_EFFECTS11)
	if (globals::features::effects11.loaded && globals::features::effects11.enableEffect)
		return false;
#endif

	return true;
}

void LinearLighting::OnWeatherColorsUpdated(RE::Sky* a_sky)
{
	if (!a_sky || !IsLinearLightingActive()) {
		weatherLightingColorsInitialized = false;
		return;
	}

	const auto effectLightingSource =
		a_sky->skyColor[static_cast<uint>(RE::TESWeather::ColorTypes::kEffectLighting)];
	const auto skyStaticsSource =
		a_sky->skyColor[static_cast<uint>(RE::TESWeather::ColorTypes::kSkyStatics)];
	const float saturation = Util::ClampFiniteOrDefault(settings.conversionSaturation, kSaturationMin, kSaturationMax, Settings{}.conversionSaturation);
	if (weatherLightingColorsInitialized &&
		saturation == weatherConversionSaturation &&
		effectLightingSource == weatherEffectLightingSource &&
		skyStaticsSource == weatherSkyStaticsSource)
		return;

	weatherEffectLightingSource = effectLightingSource;
	weatherSkyStaticsSource = skyStaticsSource;
	weatherConversionSaturation = saturation;
	effectLightingColor = DecodeAuthoredColor(effectLightingSource, saturation);
	skyStaticsColor = DecodeAuthoredColor(skyStaticsSource, saturation);
	weatherLightingColorsInitialized = true;
}

RE::NiColor LinearLighting::DecodeAuthoredColor(RE::NiColor inColor, float saturation)
{
	const auto decode = [](float value) {
		const float magnitude = std::abs(value);
		const float linear = magnitude <= kSRGBEncodedThreshold ? magnitude / kSRGBLinearScale : std::pow((magnitude + kSRGBOffset) / kSRGBScale, kSRGBExponent);
		return std::copysign(linear, value);
	};
	RE::NiColor outColor;
	outColor.red = decode(inColor.red);
	outColor.green = decode(inColor.green);
	outColor.blue = decode(inColor.blue);
	if (saturation < kSaturationMax) {
		const float luminance = outColor.red * kLinearSRGBLuminanceWeights[0] + outColor.green * kLinearSRGBLuminanceWeights[1] + outColor.blue * kLinearSRGBLuminanceWeights[2];
		outColor.red = std::lerp(luminance, outColor.red, saturation);
		outColor.green = std::lerp(luminance, outColor.green, saturation);
		outColor.blue = std::lerp(luminance, outColor.blue, saturation);
	}
	return outColor;
}

void LinearLighting::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	if (!IsLinearLightingActive() || !PerGeometryCB || !globals::d3d::context)
		return;

	auto& property1 = a_pass->geometry->GetGeometryRuntimeData().shaderProperty;
	auto lightProperty = property1 && property1->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get() ? static_cast<RE::BSLightingShaderProperty*>(property1.get()) : nullptr;
	if (!lightProperty)
		return;

	PerGeometryData perGeometryData{};
	perGeometryData.emissiveMult = lightProperty->emissiveMult;
	const auto descriptor = static_cast<RE::BSLightingShader*>(a_pass->shader)->currentRawTechnique;
	using LightingFlags = SIE::ShaderCache::LightingShaderFlags;
	using enum SIE::ShaderCache::LightingShaderTechniques;
	if ((descriptor & static_cast<uint32_t>(LightingFlags::ProjectedUV)) != 0 && (descriptor & static_cast<uint32_t>(LightingFlags::TruePbr)) == 0) {
		const auto technique = static_cast<SIE::ShaderCache::LightingShaderTechniques>((descriptor >> 24) & 0x3F);
		if (technique == LODObjects || technique == LODObjectHD)
			perGeometryData.projectedMaterialColorScale = lodProjectedMaterialColorScales[technique == LODObjectHD ? 1 : 0];
||||||| 12b261724
	RE::NiColor outColor;
	outColor.red = std::pow(inColor.red, gamma);
	outColor.green = std::pow(inColor.green, gamma);
	outColor.blue = std::pow(inColor.blue, gamma);
	return outColor;
}

void LinearLighting::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	auto& property1 = a_pass->geometry->GetGeometryRuntimeData().shaderProperty;
	auto lightProperty = property1 && property1->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get() ? static_cast<RE::BSLightingShaderProperty*>(property1.get()) : nullptr;

	if (lightProperty != nullptr) {
		float emissiveMult = 1.0f;
		if (settings.enableLinearLighting) {
			emissiveMult = lightProperty->emissiveMult;
			PerGeometryData perGeometryData{};
			perGeometryData.emissiveMult = emissiveMult;
			PerGeometryCB->Update(perGeometryData);

			ID3D11Buffer* buffer = { PerGeometryCB->CB() };
			auto context = globals::d3d::context;
			context->PSSetConstantBuffers(8, 1, &buffer);
		}
=======
	if (!IsLinearLightingActive())
		return;
	lighting.directional.color = LightColorToWorking(lighting.directionalLight);
	lighting.sun.color = SRGBToWorking(lighting.sun.color);
	for (auto* moon : { &lighting.masser, &lighting.secunda }) {
		moon->color = SRGBToWorking(moon->color);
		moon->tint = SRGBToWorking(moon->tint);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	}
	PerGeometryCB->Update(perGeometryData);

	ID3D11Buffer* buffer = PerGeometryCB->CB();
	globals::d3d::context->PSSetConstantBuffers(8, 1, &buffer);
}

void LinearLighting::SRGBToWorking(float* color) const
{
	if (!IsLinearLightingActive())
		return;
	Util::ColorSpace::SRGBToLinear(color);
	if (IsACEScgActive())
		Util::ColorSpace::SRGBGamutToAP1(color);
}

RE::NiColor LinearLighting::SRGBToWorking(RE::NiColor color) const
{
	SRGBToWorking(&color.red);
	return color;
}

RE::NiColor LinearLighting::LightColorToWorking(const RE::NiLight* light, bool effect) const
{
	if (!light)
		return {};
	const auto& diffuse = light->GetLightRuntimeData().diffuse;
	if (light == workingSunlight && diffuse == workingSunlightColor)
		return workingSunlightColor;

	auto color = effect ? static_cast<const RE::NiDirectionalLight*>(light)->GetDirectionalLightRuntimeData().effectColor : diffuse;
	if (!IsLinearLightingActive())
		return color;

	struct Memo
	{
		const RE::NiLight* light = nullptr;
		RE::NiColor source{};
		RE::NiColor result{};
		uint32_t frame = 0;
		bool effect = false;
		bool aces = false;
	};
	static thread_local Memo memo;
	const bool aces = IsACEScgActive();
	const uint32_t frame = globals::state->frameCount;
	if (memo.light == light && memo.frame == frame && memo.effect == effect && memo.aces == aces && memo.source == color)
		return memo.result;

	const auto source = color;
	auto* const point = netimmerse_cast<RE::NiPointLight*>(const_cast<RE::NiLight*>(light));
	const bool linear = point && LLFCommon::RuntimeLightDataExt::Get(point)->flags.any(LightLimitFix::LightFlags::Linear);
	if (!linear)
		Util::ColorSpace::SRGBToLinear(&color.red);
	if (aces)
		Util::ColorSpace::SRGBGamutToAP1(&color.red);
	memo = { light, source, color, frame, effect, aces };
	return color;
}

void LinearLighting::SetSunlightColor(RE::NiLight* light, RE::NiColor color)
{
	if (!light)
		return;
	light->GetLightRuntimeData().diffuse = color;
	workingSunlight = light;
	workingSunlightColor = color;
}

void LinearLighting::ClearSunlightColor(const RE::NiLight* light)
{
	if (workingSunlight == light)
		workingSunlight = nullptr;
}

namespace
{
	using Group = RE::BSGraphics::ConstantGroupLevel;
	// Native SetupGeometry reuses its argument spill slots before Unmap.
	thread_local RE::BSRenderPass* geometryPass = nullptr;

	template <RE::BSShader::Type Type>
	struct SetupGeometry
	{
		static void thunk(RE::BSShader* shader, RE::BSRenderPass* pass, uint32_t renderFlags)
		{
			if (!globals::features::linearLighting.IsLinearLightingActive())
				return func(shader, pass, renderFlags);
			if constexpr (Type == RE::BSShader::Type::Lighting) {
				globals::state->permutationData.BaseTextureIsWorking = 0;
				if (pass && pass->shaderProperty && pass->shaderProperty->material) {
					const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(pass->shaderProperty->material);
					const auto target = material->diffuseRenderTargetSourceIndex;
					globals::state->permutationData.BaseTextureIsWorking = target == RE::RENDER_TARGETS::kWATER_REFLECTIONS ||
					                                                       (!globals::state->permutationData.RenderToUI && (target == RE::RENDER_TARGETS::kMAIN || target == RE::RENDER_TARGETS::kMAIN_COPY));
				}
			}
			if constexpr (Type == RE::BSShader::Type::Effect || Type == RE::BSShader::Type::Particle) {
				constexpr auto alphaBlend = static_cast<uint>(State::ExtraShaderDescriptors::SourceAlphaBlend);
				auto& descriptor = globals::state->permutationData.ExtraShaderDescriptor;
				descriptor &= ~alphaBlend;
				if (pass && pass->geometry && pass->shaderProperty &&
					!pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kPremultAlpha)) {
					const auto* alpha = pass->geometry->GetGeometryRuntimeData().alphaProperty.get();
					if (alpha && alpha->GetAlphaBlending() && alpha->GetSrcBlendMode() == RE::NiAlphaProperty::AlphaFunction::kSrcAlpha) {
						if (alpha->GetDestBlendMode() == RE::NiAlphaProperty::AlphaFunction::kInvSrcAlpha ||
							alpha->GetDestBlendMode() == RE::NiAlphaProperty::AlphaFunction::kOne)
							descriptor |= alphaBlend;
					}
				}
			}
			auto* previous = std::exchange(geometryPass, pass);
			const SKSE::stl::scope_exit restore([previous]() noexcept { geometryPass = previous; });
			func(shader, pass, renderFlags);
			if constexpr (Type == RE::BSShader::Type::Lighting) {
				constexpr auto projectedTextureIsSRGB = static_cast<uint>(State::ExtraShaderDescriptors::ProjectedTextureIsSRGB);
				auto& descriptor = globals::state->permutationData.ExtraShaderDescriptor;
				descriptor &= ~projectedTextureIsSRGB;
				if (globals::state->currentPixelDescriptor & static_cast<uint>(SIE::ShaderCache::LightingShaderFlags::ProjectedUV)) {
					if (auto* srv = globals::game::shadowState->GetRuntimeData().PSTexture[3]) {
						D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
						srv->GetDesc(&desc);
						if (DirectX::IsSRGB(desc.Format))
							descriptor |= projectedTextureIsSRGB;
					}
				}
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	float* PixelConstant(Group group, uint index)
	{
		auto& ll = globals::features::linearLighting;
		if (!ll.IsLinearLightingActive() || !globals::state->customPixelShader)
			return nullptr;
		auto* shader = *globals::game::currentPixelShader;
		if (!shader)
			return nullptr;
		auto& buffer = shader->constantBuffers[static_cast<uint>(group)];
		if (!buffer.buffer || !buffer.data)
			return nullptr;
		return reinterpret_cast<float*>(buffer.data) + static_cast<uint8_t>(shader->constantTable[index]);
	}

	void WriteRGB(Group group, uint index, const RE::NiColor& color, uint element = 0)
	{
		if (auto* value = PixelConstant(group, index)) {
			value += element * 4;
			value[0] = color.red;
			value[1] = color.green;
			value[2] = color.blue;
		}
	}

	struct LightingMaterialUpload
	{
		static void thunk(ID3D11DeviceContext* context, ID3D11Resource* buffer, UINT subresource, const RE::BSLightingShaderMaterialBase* material)
		{
			auto& ll = globals::features::linearLighting;
			const auto descriptor = globals::state->currentPixelDescriptor;
			if (material && ll.IsLinearLightingActive() &&
				(descriptor & static_cast<uint>(SIE::ShaderCache::LightingShaderFlags::Specular)) &&
				!(descriptor & static_cast<uint>(SIE::ShaderCache::LightingShaderFlags::TruePbr)))
				WriteRGB(Group::PerMaterial, 25, ll.SRGBToWorking(material->specularColor) * material->specularColorScale);
			context->Unmap(buffer, subresource);
		}
	};

	struct EffectMaterialUpload
	{
		static void thunk(ID3D11DeviceContext* context, ID3D11Resource* buffer, UINT subresource, const RE::BSEffectShaderMaterial* material)
		{
			auto& ll = globals::features::linearLighting;
			if (material && ll.IsLinearLightingActive() &&
				!(globals::state->currentPixelDescriptor & (static_cast<uint>(SIE::ShaderCache::EffectShaderFlags::GrayscaleToColor) |
															   static_cast<uint>(SIE::ShaderCache::EffectShaderFlags::Blood)))) {
				const auto& c = material->baseColor;
				WriteRGB(Group::PerMaterial, 15, ll.SRGBToWorking({ c.red, c.green, c.blue }) * material->baseColorScale);
			}
			context->Unmap(buffer, subresource);
		}
	};

	struct LightingGeometryUpload
	{
		static void thunk(ID3D11DeviceContext* context, ID3D11Resource* buffer)
		{
			auto* pass = geometryPass;
			auto& ll = globals::features::linearLighting;
			if (pass && ll.IsLinearLightingActive()) {
				const auto* property = static_cast<const RE::BSLightingShaderProperty*>(pass->shaderProperty);
				if (property && property->emissiveColor)
					WriteRGB(Group::PerGeometry, 8, ll.SRGBToWorking(*property->emissiveColor) * (property->emissiveMult * ll.settings.emitColorMult));
				if (pass->sceneLights && pass->numLights && pass->sceneLights[0] && pass->sceneLights[0]->light) {
					const auto* light = pass->sceneLights[0]->light.get();
					const auto& data = light->GetLightRuntimeData();
					const float scale = RE::ImageSpaceManager::GetSingleton()->GetRuntimeData().data.baseData.hdr.sunlightScale;
					WriteRGB(Group::PerGeometry, 4, ll.LightColorToWorking(light) * (data.fade * scale));
				}
				if (pass->sceneLights && !globals::features::lightLimitFix.loaded) {
					for (uint i = 1; i < std::min<uint>(pass->numLights, 8); ++i) {
						const auto* bsLight = pass->sceneLights[i];
						if (!bsLight || !bsLight->light)
							continue;
						const auto* light = bsLight->light.get();
						const auto& data = light->GetLightRuntimeData();
						WriteRGB(Group::PerGeometry, 2, ll.LightColorToWorking(light) * (data.fade * bsLight->lodDimmer), i - 1);
					}
				}
			}
			context->Unmap(buffer, 0);
		}
	};

	struct EffectGeometryUpload
	{
		static void thunk(ID3D11DeviceContext* context, ID3D11Resource* buffer)
		{
			auto* pass = geometryPass;
			auto& ll = globals::features::linearLighting;
			if (pass && pass->shaderProperty && ll.IsLinearLightingActive()) {
				auto* property = pass->shaderProperty;
				const auto descriptor = globals::state->currentPixelDescriptor;
				const bool membrane = descriptor & static_cast<uint>(SIE::ShaderCache::EffectShaderFlags::Membrane);
				const bool grayscale = descriptor & static_cast<uint>(SIE::ShaderCache::EffectShaderFlags::GrayscaleToColor);
				if (membrane && property->effectData && !grayscale) {
					const auto& data = *property->effectData;
					WriteRGB(Group::PerGeometry, 0, ll.SRGBToWorking({ data.fillColor.red, data.fillColor.green, data.fillColor.blue }) * data.baseFillScale);
				} else if (!membrane && !grayscale && property->GetRTTI() == globals::rtti::BSEffectShaderPropertyRTTI.get()) {
					const auto* effect = static_cast<const RE::BSEffectShaderProperty*>(property);
					WriteRGB(Group::PerGeometry, 0, ll.SRGBToWorking(effect->emittanceColor ? *effect->emittanceColor : RE::NiColor{ 1.f, 1.f, 1.f }));
				}
				if (!membrane && (descriptor & (1u << 16)) && pass->numLights && pass->sceneLights && pass->sceneLights[0]) {
					if (const auto* light = netimmerse_cast<RE::NiDirectionalLight*>(pass->sceneLights[0]->light.get())) {
						const float scale = RE::ImageSpaceManager::GetSingleton()->GetRuntimeData().data.baseData.hdr.sunlightScale;
						WriteRGB(Group::PerGeometry, 11, ll.LightColorToWorking(light, true) * (light->GetLightRuntimeData().fade * scale));
					}
					for (uint i = 1; i < std::min<uint>(pass->numLights, 5); ++i) {
						const auto* bsLight = pass->sceneLights[i];
						if (!bsLight || !bsLight->light)
							continue;
						const auto* light = bsLight->light.get();
						const auto& data = light->GetLightRuntimeData();
						const auto color = ll.LightColorToWorking(light) * (data.fade * bsLight->lodDimmer);
						if (auto* r = PixelConstant(Group::PerGeometry, 8))
							r[i - 1] = color.red;
						if (auto* g = PixelConstant(Group::PerGeometry, 9))
							g[i - 1] = color.green;
						if (auto* b = PixelConstant(Group::PerGeometry, 10))
							b[i - 1] = color.blue;
					}
				}
			}
			context->Unmap(buffer, 0);
		}
	};

	struct WaterMaterialUpload
	{
		static void thunk(ID3D11DeviceContext* context, ID3D11Resource* buffer, UINT subresource, const RE::BSWaterShaderMaterial* material)
		{
			auto& ll = globals::features::linearLighting;
			if (material && ll.IsLinearLightingActive()) {
				static REL::Relocation<const RE::NiColor*> waterLighting{ REL::RelocationID(527901, 414848) };
				const auto lighting = ll.SRGBToWorking(*waterLighting);
				const auto& deep = material->deepWaterColor;
				const auto& reflection = material->reflectionColor;
				WriteRGB(Group::PerMaterial, 1, ll.SRGBToWorking(material->shallowWaterColor) * lighting);
				WriteRGB(Group::PerMaterial, 2, ll.SRGBToWorking({ deep.red, deep.green, deep.blue }) * lighting);
				WriteRGB(Group::PerMaterial, 3, ll.SRGBToWorking({ reflection.red, reflection.green, reflection.blue }) * lighting);
			}
			context->Unmap(buffer, subresource);
		}
	};

	struct WaterGeometryUpload
	{
		static void thunk(ID3D11DeviceContext* context, ID3D11Resource* buffer)
		{
			auto* pass = geometryPass;
			auto& ll = globals::features::linearLighting;
			const uint lightCount = (globals::state->currentPixelDescriptor >> 11) & 0xF;
			if (pass && pass->shaderProperty && pass->sceneLights && ll.IsLinearLightingActive() && lightCount < 8) {
				const auto* material = static_cast<const RE::BSWaterShaderMaterial*>(pass->shaderProperty->material);
				for (uint i = 0; material && i < lightCount && i + 1 < pass->numLights; ++i) {
					const auto* bsLight = pass->sceneLights[i + 1];
					if (!bsLight || !bsLight->light || !bsLight->affectWater)
						continue;
					const auto* light = bsLight->light.get();
					const auto& data = light->GetLightRuntimeData();
					const float lightScale = *reinterpret_cast<const float*>(reinterpret_cast<const std::byte*>(material) + 0xAC);
					WriteRGB(Group::PerGeometry, 18, ll.LightColorToWorking(light) * (data.fade * bsLight->lodDimmer * lightScale), i);
				}
			}
			context->Unmap(buffer, 0);
		}
	};

	struct SetDirectionalAmbientColors
	{
		static void thunk(Effects11::DirectionalAmbientColors& DirectionalAmbientColors, RE::NiColor* AmbientSpecularTint, float AmbientSpecularFresnel)
		{
			auto& linearLighting = globals::features::linearLighting;
			if (linearLighting.IsLinearLightingActive()) {
				Effects11::DirectionalAmbientColors converted = DirectionalAmbientColors;
				for (auto& axis : converted.directionalAmbientColors) {
					for (auto& color : axis)
						color = linearLighting.SRGBToWorking(color);
				}

				RE::NiColor convertedSpecularTint{};
				auto* specularTint = AmbientSpecularTint;
				if (AmbientSpecularTint) {
					convertedSpecularTint = linearLighting.SRGBToWorking(*AmbientSpecularTint);
					specularTint = &convertedSpecularTint;
				}

				func(converted, specularTint, AmbientSpecularFresnel);
				return;
			}
			func(DirectionalAmbientColors, AmbientSpecularTint, AmbientSpecularFresnel);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct RenderMenuScene
	{
		static DWORD thunk(RE::UI3DSceneManager* manager, int scheme, RE::NiCamera* camera, bool arg4)
		{
			if (!globals::features::linearLighting.IsLinearLightingActive())
				return func(manager, scheme, camera, arg4);
			auto& renderToUI = globals::state->permutationData.RenderToUI;
			const uint previous = renderToUI;
			renderToUI = 1;
			const auto result = func(manager, scheme, camera, arg4);
			renderToUI = previous;
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

}

namespace ImageSpaceColorManagement
{
	constexpr std::size_t VOLUMETRIC_LIGHTING_COLOR = 0;

	template <std::size_t... ColorOffsets>
	class ScopedInputColors
	{
	public:
		explicit ScopedInputColors(RE::ImageSpaceEffectParam* param)
		{
			shaderParam = skyrim_cast<RE::ImageSpaceShaderParam*>(param);
			if (!globals::features::linearLighting.IsLinearLightingActive() || !globals::shaderCache->IsEnabled() ||
				!globals::state->enablePShaders || !globals::state->ShaderEnabled(RE::BSShader::Type::ImageSpace) ||
				!shaderParam || !shaderParam->pixelConstantGroup ||
				!((ColorOffsets + 3 <= shaderParam->pixelConstantGroupSize) && ...))
				return;

			constexpr std::array offsets{ ColorOffsets... };
			for (std::size_t index = 0; index < offsets.size(); ++index) {
				auto* color = shaderParam->pixelConstantGroup + offsets[index];
				std::copy_n(color, 3, originalColors[index].data());
				auto& linearLighting = globals::features::linearLighting;
				linearLighting.SRGBToWorking(color);
			}
			active = true;
		}

		~ScopedInputColors()
		{
			if (!active)
				return;

			constexpr std::array offsets{ ColorOffsets... };
			for (std::size_t index = 0; index < offsets.size(); ++index)
				std::copy_n(originalColors[index].data(), 3, shaderParam->pixelConstantGroup + offsets[index]);
		}

	private:
		RE::ImageSpaceShaderParam* shaderParam = nullptr;
		std::array<std::array<float, 3>, sizeof...(ColorOffsets)> originalColors{};
		bool active = false;
	};

	template <RE::ImageSpaceManager::ImageSpaceEffectEnum EffectType, std::size_t... ColorOffsets>
	struct BSImagespaceShader_Render
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
		{
			const ScopedInputColors<ColorOffsets...> inputColors(param);
			func(imageSpaceShader, shape, param);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void LinearLighting::Load()
{
	ApplyGameGamma();
	// Matches Reset() so a saved Vanilla mode does not cost a shader reload on the first frame.
	configuredLinearLighting = loaded && settings.enableLinearLighting && globals::features::postProcessing.loaded &&
	                           !PostProcessingMode::IsVanillaPending();
	configuredACEScg = configuredLinearLighting && settings.enableACEScg;

	// Installed regardless of the toggle so Reset() can enable Linear Lighting without a restart.
	struct UploadCall : Xbyak::CodeGenerator
	{
		UploadCall(uintptr_t target, bool waterMaterial = false)
		{
			if (waterMaterial && REL::Module::IsAE())
				mov(r9, r15);
			else
				mov(r9, rbx);
			xor_(r8d, r8d);
			mov(rax, target);
			jmp(rax);
		}
	};
	static UploadCall lightingMaterial(reinterpret_cast<uintptr_t>(LightingMaterialUpload::thunk));
	static UploadCall effectMaterial(reinterpret_cast<uintptr_t>(EffectMaterialUpload::thunk));
	static UploadCall waterMaterial(reinterpret_cast<uintptr_t>(WaterMaterialUpload::thunk), true);
	const std::array calls{
		std::tuple{ "Lighting material"sv, REL::RelocationID(100563, 107298).address() + REL::Relocate(0xACD, 0xC65), lightingMaterial.getCode() },
		std::tuple{ "Effect material"sv, REL::RelocationID(100744, 107525).address() + REL::Relocate(0x3E2, 0x3E3), effectMaterial.getCode() },
		std::tuple{ "Lighting geometry"sv, REL::RelocationID(100565, 107300).address() + Util::VersionedRelocation::Select(0xC1E, 0x12F0, 0x1312), reinterpret_cast<const uint8_t*>(LightingGeometryUpload::thunk) },
		std::tuple{ "Effect geometry"sv, REL::RelocationID(100746, 107527).address() + REL::Relocate(0xF16, 0xE6F), reinterpret_cast<const uint8_t*>(EffectGeometryUpload::thunk) },
		std::tuple{ "Water material"sv, REL::RelocationID(100602, 107363).address() + REL::Relocate(0x5FA, 0x626), waterMaterial.getCode() }
	};
	const auto verify = [](std::string_view name, uintptr_t address, const auto& expected) {
		const auto* actual = reinterpret_cast<const uint8_t*>(address);
		if (std::memcmp(actual, expected.data(), expected.size()) == 0)
			return;
		auto message = fmt::format("Linear Lighting: {} does not match the verified layout.\nSkyrim {}, RVA 0x{:X}\nExpected:",
			name, Util::GetFormattedVersion(REL::Module::get().version()), address - REL::Module::get().base());
		for (const auto byte : expected)
			message += fmt::format(" {:02X}", byte);
		message += "\nActual:";
		for (std::size_t i = 0; i < expected.size(); ++i)
			message += fmt::format(" {:02X}", actual[i]);
		stl::report_and_fail(message);
	};
	constexpr std::array<uint8_t, 6> unmapCall{ 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x78 };
	for (const auto& [name, address, code] : calls)
		verify(name, address, unmapCall);
	const auto waterGeometryAddress = REL::RelocationID(100604, 107365).address() + REL::Relocate(0x52F, 0x61F);
	constexpr std::array<uint8_t, 6> waterUnmapCall{ 0x45, 0x33, 0xC0, 0xFF, 0x50, 0x78 };
	verify("Water geometry", waterGeometryAddress, waterUnmapCall);
	const auto menuAddress = REL::RelocationID(51855, 52727).address();
	constexpr std::array<uint8_t, 16> menuEntry{ 0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 };
	verify("Menu renderer", menuAddress, menuEntry);
	RenderMenuScene::func = menuAddress;
	if (DetourTransactionBegin() != NO_ERROR)
		stl::report_and_fail("Linear Lighting: could not begin menu hook installation."sv);
	if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
		DetourAttach(reinterpret_cast<PVOID*>(&RenderMenuScene::func), reinterpret_cast<PVOID>(RenderMenuScene::thunk)) != NO_ERROR) {
		DetourTransactionAbort();
		stl::report_and_fail("Linear Lighting: could not attach the menu hook."sv);
	}
	if (DetourTransactionCommit() != NO_ERROR)
		stl::report_and_fail("Linear Lighting: could not commit the menu hook."sv);
	stl::write_vfunc<0x1,
		ImageSpaceColorManagement::BSImagespaceShader_Render<RE::ImageSpaceManager::ISCompositeVolumetricLighting,
			ImageSpaceColorManagement::VOLUMETRIC_LIGHTING_COLOR>>(
		RE::VTABLE_BSImagespaceShaderISCompositeVolumetricLighting[3]);
	stl::write_vfunc<0x1,
		ImageSpaceColorManagement::BSImagespaceShader_Render<RE::ImageSpaceManager::ISCompositeLensFlareVolumetricLighting,
			ImageSpaceColorManagement::VOLUMETRIC_LIGHTING_COLOR>>(
		RE::VTABLE_BSImagespaceShaderISCompositeLensFlareVolumetricLighting[3]);
	stl::detour_thunk<SetDirectionalAmbientColors>(REL::RelocationID(98989, 105643));
	stl::write_vfunc<0x6, SetupGeometry<RE::BSShader::Type::Lighting>>(RE::VTABLE_BSLightingShader[0]);
	stl::write_vfunc<0x6, SetupGeometry<RE::BSShader::Type::Effect>>(RE::VTABLE_BSEffectShader[0]);
	stl::write_vfunc<0x6, SetupGeometry<RE::BSShader::Type::Particle>>(RE::VTABLE_BSParticleShader[0]);
	stl::write_vfunc<0x6, SetupGeometry<RE::BSShader::Type::Water>>(RE::VTABLE_BSWaterShader[0]);
	SKSE::GetTrampoline().write_call<6>(waterGeometryAddress, reinterpret_cast<uintptr_t>(WaterGeometryUpload::thunk));
	for (const auto& [name, address, code] : calls)
		SKSE::GetTrampoline().write_call<6>(address, reinterpret_cast<uintptr_t>(code));
}

#undef I18N_KEY_PREFIX

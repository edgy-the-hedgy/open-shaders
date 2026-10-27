#include "RedstoneGI.h"
#include "RuntimeDenoiser.h"
#include "Globals.h"
#include "Utils/UI.h"

namespace
{
	constexpr float kILRadiusMin = 512.0f;
	constexpr float kILRadiusMax = 8192.0f;
	constexpr float kILRadiusDefault = 4096.0f;
	constexpr float kContributionMinEV = -8.0f;
	constexpr float kContributionMaxEV = 8.0f;
	constexpr float kMinimumRoughnessFloor = 0.02f;

	struct RedstoneConfiguration : ScreenSpaceGI::Settings
	{
		RedstoneConfiguration()
		{
			EnableGI = true;
			EnableExperimentalSpecularGI = true;
			ResourceProfile = ScreenSpaceGI::kResourceProfileFullGI;
			GIRadius = kILRadiusDefault;
			ResolutionMode = 1;
			EnableAdaptiveSampling = true;
		}
		bool RREnabled = true;
		bool DiffuseEnabled = true;
		bool SpecularEnabled = true;
		bool RRCheckerboard = true;
		float ContributionEV = 0.0f;
		float SpecularContributionEV = 0.0f;
		float DiffuseContributionEV = 0.0f;
		uint DiffuseSlices = 2;
		uint DiffuseSteps = 10;
		uint SpecularSlices = 2;
		uint SpecularSteps = 10;
		float ILCompensationDistance = 256.0f;
		float SpecularRoughnessFloor = 0.2f;
		bool DiffuseOcclusionEnabled = false;
		bool SpecularOcclusionEnabled = false;
		uint DiffuseOcclusionSlices = 2;
		uint SpecularOcclusionSlices = 2;
		uint DiffuseOcclusionSteps = 10;
		uint SpecularOcclusionSteps = 10;
		float DiffuseOcclusionRadius = 256.0f;
		float SpecularOcclusionRadius = 256.0f;
		float DiffuseOcclusionStrength = 1.0f;
		float SpecularOcclusionStrength = 1.0f;

	};

	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RedstoneConfiguration,
		Enabled, EnableGI, EnableExperimentalSpecularGI, EnableVanillaSSAO,
		ResolutionMode, ResourceProfile, EnableAdaptiveSampling,
		MinScreenRadius, GIRadius, DepthFadeRange,
		GISaturation, GIDistanceCompensation, GIStrength,
		RREnabled, DiffuseEnabled, SpecularEnabled, RRCheckerboard, ContributionEV, SpecularContributionEV, DiffuseContributionEV,
		DiffuseSlices, DiffuseSteps, SpecularSlices, SpecularSteps, ILCompensationDistance, SpecularRoughnessFloor,
		DiffuseOcclusionEnabled, SpecularOcclusionEnabled, DiffuseOcclusionSlices, SpecularOcclusionSlices, DiffuseOcclusionSteps, SpecularOcclusionSteps, DiffuseOcclusionRadius, SpecularOcclusionRadius, DiffuseOcclusionStrength, SpecularOcclusionStrength)
}

RedstoneGI::RedstoneGI(RedstoneDenoiser& a_denoiser) : denoiser(a_denoiser)
{
	RestoreDefaultSettings();
}

json RedstoneGI::DefaultRedstoneSettings()
{
	return RedstoneConfiguration{};
}

void RedstoneGI::EnforceFixedPath()
{
	settings.Enabled = true;
	settings.EnableGI = true;
	settings.EnableExperimentalSpecularGI = true;
	settings.ResourceProfile = kResourceProfileFullGI;
	settings.GIRadius = std::isfinite(settings.GIRadius) ? std::clamp(settings.GIRadius, kILRadiusMin, kILRadiusMax) : kILRadiusDefault;
	settings.ResolutionMode = std::clamp(settings.ResolutionMode, 0, 2);
}

void RedstoneGI::LoadRedstoneSettings(const json& a_json)
{
	const auto selected = a_json.get<RedstoneConfiguration>();
	settings = selected;
	const auto sanitizeEV = [](float a_value) {
		return std::isfinite(a_value) ? std::clamp(a_value, kContributionMinEV, kContributionMaxEV) : 0.0f;
	};
	rgiContributionEV = sanitizeEV(selected.ContributionEV);
	specularContributionEV = sanitizeEV(selected.SpecularContributionEV);
	diffuseContributionEV = sanitizeEV(selected.DiffuseContributionEV);
	diffuseSlices = std::clamp(selected.DiffuseSlices, 1u, 10u);
	diffuseSteps = std::clamp(selected.DiffuseSteps, 1u, 20u);
	specularSlices = std::clamp(selected.SpecularSlices, 1u, 10u);
	specularSteps = std::clamp(selected.SpecularSteps, 1u, 20u);
	const float compensationDistance = a_json.value("ILCompensationDistance", a_json.value("AORadius", selected.ILCompensationDistance));
	giCompensationMaxDistance = std::isfinite(compensationDistance) ? std::clamp(compensationDistance, 10.0f, kILRadiusMax) : 256.0f;
	specularRoughnessFloor = std::isfinite(selected.SpecularRoughnessFloor) ? std::clamp(selected.SpecularRoughnessFloor, kMinimumRoughnessFloor, 1.0f) : 0.2f;
	occlusion.diffuseEnabled = selected.DiffuseOcclusionEnabled;
	occlusion.specularEnabled = selected.SpecularOcclusionEnabled;
	occlusion.diffuseSlices = std::clamp(selected.DiffuseOcclusionSlices, 1u, 10u);
	occlusion.specularSlices = std::clamp(selected.SpecularOcclusionSlices, 1u, 10u);
	occlusion.diffuseSteps = std::clamp(selected.DiffuseOcclusionSteps, 1u, 20u);
	occlusion.specularSteps = std::clamp(selected.SpecularOcclusionSteps, 1u, 20u);
	occlusion.diffuseRadius = std::isfinite(selected.DiffuseOcclusionRadius) ? std::clamp(selected.DiffuseOcclusionRadius, 16.0f, 8192.0f) : 256.0f;
	occlusion.specularRadius = std::isfinite(selected.SpecularOcclusionRadius) ? std::clamp(selected.SpecularOcclusionRadius, 16.0f, 8192.0f) : 256.0f;
	occlusion.diffuseStrength = std::isfinite(selected.DiffuseOcclusionStrength) ? std::clamp(selected.DiffuseOcclusionStrength, 0.0f, 1.0f) : 1.0f;
	occlusion.specularStrength = std::isfinite(selected.SpecularOcclusionStrength) ? std::clamp(selected.SpecularOcclusionStrength, 0.0f, 1.0f) : 1.0f;
	rrEnabled = selected.RREnabled;
	diffuseEnabled = selected.DiffuseEnabled;
	specularEnabled = selected.SpecularEnabled;
	SetRRCheckerboard(selected.RRCheckerboard);
	denoiser.SetConfiguration(a_json.value("RRConfiguration", json::object()));
	EnforceFixedPath();
	recompileFlag = true;
	QueueHistoryReset();
}

void RedstoneGI::SaveRedstoneSettings(json& a_json) const
{
	RedstoneConfiguration selected;
	static_cast<ScreenSpaceGI::Settings&>(selected) = settings;
	selected.RREnabled = rrEnabled;
	selected.DiffuseEnabled = diffuseEnabled;
	selected.SpecularEnabled = specularEnabled;
	selected.RRCheckerboard = rrCheckerboardEnabled;
	selected.ContributionEV = rgiContributionEV;
	selected.SpecularContributionEV = specularContributionEV;
	selected.DiffuseContributionEV = diffuseContributionEV;
	selected.DiffuseSlices = diffuseSlices;
	selected.DiffuseSteps = diffuseSteps;
	selected.SpecularSlices = specularSlices;
	selected.SpecularSteps = specularSteps;
	selected.ILCompensationDistance = giCompensationMaxDistance;
	selected.SpecularRoughnessFloor = specularRoughnessFloor;
	selected.DiffuseOcclusionEnabled = occlusion.diffuseEnabled;
	selected.SpecularOcclusionEnabled = occlusion.specularEnabled;
	selected.DiffuseOcclusionSlices = occlusion.diffuseSlices;
	selected.SpecularOcclusionSlices = occlusion.specularSlices;
	selected.DiffuseOcclusionSteps = occlusion.diffuseSteps;
	selected.SpecularOcclusionSteps = occlusion.specularSteps;
	selected.DiffuseOcclusionRadius = occlusion.diffuseRadius;
	selected.SpecularOcclusionRadius = occlusion.specularRadius;
	selected.DiffuseOcclusionStrength = occlusion.diffuseStrength;
	selected.SpecularOcclusionStrength = occlusion.specularStrength;
	a_json = selected;
	a_json["RRConfiguration"] = denoiser.GetConfiguration().at("overrides");
}

void RedstoneGI::RestoreDefaultSettings()
{
	settings = RedstoneConfiguration{};
	rgiContributionEV = specularContributionEV = diffuseContributionEV = 0.0f;
	diffuseSlices = specularSlices = 2;
	diffuseSteps = specularSteps = 10;
	giCompensationMaxDistance = 256.0f;
	specularRoughnessFloor = 0.2f;
	rrEnabled = diffuseEnabled = specularEnabled = true;
	occlusion = OcclusionSettings{};
	denoiser.SetConfiguration(json::object());
	SetRRCheckerboard(true);
	EnforceFixedPath();
	recompileFlag = true;
	QueueHistoryReset();
}

void RedstoneGI::LoadSettings(json& a_json)
{
	LoadRedstoneSettings(a_json);
}

void RedstoneGI::SaveSettings(json& a_json)
{
	SaveRedstoneSettings(a_json);
}

const void* RedstoneGI::GetSettingsBlob() const
{
	return nullptr;
}

const void* RedstoneGI::GetBootValue(std::string_view) const
{
	return nullptr;
}

void RedstoneGI::SetRRCheckerboard(bool a_enabled)
{
	if (rrCheckerboardEnabled == a_enabled)
		return;
	rrCheckerboardEnabled = a_enabled;
	rrDepthHistoryValid = false;
	QueueHistoryReset();
}

void RedstoneGI::DrawRayRegenerationSettings()
{
	if (ImGui::Checkbox(T("feature.upscaling.redstone.rr_enabled", "FSR-RR"), &rrEnabled)) QueueHistoryReset();
	DrawRRDebugSettings();
	denoiser.DrawConfigurationSettings();
	bool checkerboard = rrCheckerboardEnabled;
	if (ImGui::Checkbox(T("feature.upscaling.redstone.checkerboard", "RR Checkerboard Sampling"), &checkerboard))
		SetRRCheckerboard(checkerboard);
	if (ImGui::BeginTable("RedstoneGIResolution", 3)) {
		ImGui::TableNextColumn();
		recompileFlag |= ImGui::RadioButton(T("feature.upscaling.redstone.full_res", "Full Res"), &settings.ResolutionMode, 0);
		ImGui::TableNextColumn();
		recompileFlag |= ImGui::RadioButton(T("feature.upscaling.redstone.half_res", "Half Res"), &settings.ResolutionMode, 1);
		ImGui::TableNextColumn();
		recompileFlag |= ImGui::RadioButton(T("feature.upscaling.redstone.quarter_res", "Quarter Res"), &settings.ResolutionMode, 2);
		ImGui::EndTable();
	}
}

void RedstoneGI::DrawAmbientOcclusionSettings()
{
	ImGui::Checkbox(T("feature.upscaling.redstone.vanilla_ssao", "Vanilla SSAO"), &settings.EnableVanillaSSAO);
	if (ImGui::BeginTable("RedstoneOcclusionSignals", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
		ImGui::TableNextColumn();
		if (ImGui::TreeNodeEx("Diffuse AO", ImGuiTreeNodeFlags_DefaultOpen)) {
			bool enabled = occlusion.diffuseEnabled != 0;
			if (ImGui::Checkbox("Enabled##diffuseOcclusion", &enabled)) { occlusion.diffuseEnabled = enabled; QueueHistoryReset(); }
			ImGui::SliderInt("Slices##diffuseOcclusion", reinterpret_cast<int*>(&occlusion.diffuseSlices), 1, 10, "%d", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderInt("Steps##diffuseOcclusion", reinterpret_cast<int*>(&occlusion.diffuseSteps), 1, 20, "%d", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat("Radius##diffuseOcclusion", &occlusion.diffuseRadius, 16.0f, 8192.0f, "%.1f units", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat("Strength##diffuseOcclusion", &occlusion.diffuseStrength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::TreePop();
		}
		ImGui::TableNextColumn();
		if (ImGui::TreeNodeEx("Specular Occlusion", ImGuiTreeNodeFlags_DefaultOpen)) {
			bool enabled = occlusion.specularEnabled != 0;
			if (ImGui::Checkbox("Enabled##specularOcclusion", &enabled)) { occlusion.specularEnabled = enabled; QueueHistoryReset(); }
			ImGui::SliderInt("Slices##specularOcclusion", reinterpret_cast<int*>(&occlusion.specularSlices), 1, 10, "%d", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderInt("Steps##specularOcclusion", reinterpret_cast<int*>(&occlusion.specularSteps), 1, 20, "%d", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat("Radius##specularOcclusion", &occlusion.specularRadius, 16.0f, 8192.0f, "%.1f units", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat("Strength##specularOcclusion", &occlusion.specularStrength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::TreePop();
		}

		ImGui::EndTable();
	}
}

void RedstoneGI::DrawSettings()
{
	DrawRayRegenerationSettings();
	ImGui::SeparatorText("RedstoneGI");
	ImGui::SliderFloat(T("feature.upscaling.redstone.contribution", "RGI Contribution (EV)"), &rgiContributionEV, kContributionMinEV, kContributionMaxEV, "%.1f", ImGuiSliderFlags_AlwaysClamp);
	recompileFlag |= ImGui::Checkbox(T("feature.upscaling.redstone.adaptive", "Adaptive Sampling"), &settings.EnableAdaptiveSampling);

	ImGui::SliderFloat(T("feature.upscaling.redstone.il_brightness", "IL Source Brightness"), &settings.GIStrength, 0.f, 6.f, "%.2f");
	ImGui::SliderFloat(T("feature.upscaling.redstone.il_radius", "IL radius"), &settings.GIRadius, kILRadiusMin, kILRadiusMax, "%.1f units", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T("feature.upscaling.redstone.min_radius", "Min Screen Radius"), &settings.MinScreenRadius, 0.f, 0.05f, "%.3f");
	ImGui::SliderFloat2(T("feature.upscaling.redstone.depth_fade", "Depth Fade Range"), &settings.DepthFadeRange.x, 1e4f, 5e4f, "%.0f units");
	ImGui::SliderFloat(T("feature.upscaling.redstone.il_compensation", "IL Distance Compensation"), &settings.GIDistanceCompensation, -5.f, 5.f, "%.1f");
	ImGui::SliderFloat(T("feature.upscaling.redstone.il_compensation_distance", "IL Compensation Distance"), &giCompensationMaxDistance, 10.f, kILRadiusMax, "%.1f units", ImGuiSliderFlags_AlwaysClamp);

	if (ImGui::BeginTable("RedstoneLightingSignals", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
		ImGui::TableNextColumn();
		if (ImGui::TreeNodeEx(T("feature.upscaling.redstone.diffuse_il", "Diffuse IL"), ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::Checkbox("Enabled##diffuseIL", &diffuseEnabled)) QueueHistoryReset();
			ImGui::SliderInt(T("feature.upscaling.redstone.diffuse_slices", "Diffuse Slices"), reinterpret_cast<int*>(&diffuseSlices), 1, 10, "%d", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderInt(T("feature.upscaling.redstone.diffuse_steps", "Diffuse Steps Per Slice"), reinterpret_cast<int*>(&diffuseSteps), 1, 20, "%d", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T("feature.upscaling.redstone.diffuse_contribution", "Diffuse IL Contribution (EV)"), &diffuseContributionEV, kContributionMinEV, kContributionMaxEV, "%.1f", ImGuiSliderFlags_AlwaysClamp);
			Util::PercentageSlider(T("feature.upscaling.redstone.il_saturation", "IL Saturation"), &settings.GISaturation);
			ImGui::TreePop();
		}

		ImGui::TableNextColumn();
		if (ImGui::TreeNodeEx(T("feature.upscaling.redstone.specular_il", "Specular IL"), ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::Checkbox("Enabled##specularIL", &specularEnabled)) QueueHistoryReset();
			ImGui::SliderInt(T("feature.upscaling.redstone.specular_slices", "Specular Slices"), reinterpret_cast<int*>(&specularSlices), 1, 10, "%d", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderInt(T("feature.upscaling.redstone.specular_steps", "Specular Steps Per Slice"), reinterpret_cast<int*>(&specularSteps), 1, 20, "%d", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T("feature.upscaling.redstone.specular_contribution", "Specular IL Contribution (EV)"), &specularContributionEV, kContributionMinEV, kContributionMaxEV, "%.1f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat(T("feature.upscaling.redstone.roughness_floor", "Specular Roughness Floor"), &specularRoughnessFloor, kMinimumRoughnessFloor, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::TreePop();
		}

		ImGui::EndTable();
	}

	if (ImGui::TreeNodeEx(T("feature.upscaling.redstone.ambient_occlusion", "Ambient Occlusion"), ImGuiTreeNodeFlags_DefaultOpen)) {
		DrawAmbientOcclusionSettings();
		ImGui::TreePop();
	}

	if (ImGui::TreeNode(T("feature.upscaling.redstone.buffer_viewer", "Buffer Viewer"))) {
		static float debugRescale = 0.3f;
		ImGui::SliderFloat(T("feature.upscaling.redstone.view_resize", "View Resize"), &debugRescale, 0.f, 1.f);
		BUFFER_VIEWER_NODE(texNoise, debugRescale)
		BUFFER_VIEWER_NODE(texWorkingDepth, debugRescale)
		BUFFER_VIEWER_NODE(texRadiance, debugRescale)
		BUFFER_VIEWER_NODE(texRRDiffuseInput, debugRescale)
		BUFFER_VIEWER_NODE(texRRSpecularInput, debugRescale)
		if (occlusion.diffuseEnabled) {
			BUFFER_VIEWER_NODE(texDiffuseOcclusionInput, debugRescale)
			if (rrProviderSucceeded) { BUFFER_VIEWER_NODE(texDiffuseOcclusionOutput, debugRescale) }
		}
		if (occlusion.specularEnabled) {
			BUFFER_VIEWER_NODE(texSpecularOcclusionInput, debugRescale)
			if (rrProviderSucceeded) { BUFFER_VIEWER_NODE(texSpecularOcclusionOutput, debugRescale) }
		}
		if (occlusion.diffuseEnabled || occlusion.specularEnabled) { BUFFER_VIEWER_NODE(texPackedOcclusion, debugRescale) }
		ImGui::TreePop();
	}
}

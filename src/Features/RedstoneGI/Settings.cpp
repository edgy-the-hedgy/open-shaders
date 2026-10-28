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

	struct RedstoneConfiguration : RedstoneGI::Settings
	{
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
		float OccluderDepth = RedstoneGI::kDefaultOccluderDepth;
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
		EnableVanillaSSAO,
		ResolutionMode, EnableAdaptiveSampling,
		MinScreenRadius, GIRadius, DepthFadeRange,
		GISaturation, GIDistanceCompensation, GIStrength,
		RREnabled, DiffuseEnabled, SpecularEnabled, RRCheckerboard, ContributionEV, SpecularContributionEV, DiffuseContributionEV,
		DiffuseSlices, DiffuseSteps, SpecularSlices, SpecularSteps, OccluderDepth, ILCompensationDistance, SpecularRoughnessFloor,
		DiffuseOcclusionEnabled, SpecularOcclusionEnabled, DiffuseOcclusionSlices, SpecularOcclusionSlices, DiffuseOcclusionSteps, SpecularOcclusionSteps, DiffuseOcclusionRadius, SpecularOcclusionRadius, DiffuseOcclusionStrength, SpecularOcclusionStrength)
}

RedstoneGI::RedstoneGI()
{
	RestoreDefaultSettings();
}

json RedstoneGI::DefaultRedstoneSettings()
{
	return RedstoneConfiguration{};
}

void RedstoneGI::EnforceFixedPath()
{
	settings.GIRadius = std::isfinite(settings.GIRadius) ? std::clamp(settings.GIRadius, kILRadiusMin, kILRadiusMax) : kILRadiusDefault;
	settings.ResolutionMode = std::clamp(settings.ResolutionMode, 0, 2);
}

void RedstoneGI::LoadRedstoneSettings(const json& a_json)
{
	const auto configuration = a_json.get<RedstoneConfiguration>();
	settings = configuration;
	const auto sanitizeEV = [](float a_value) {
		return std::isfinite(a_value) ? std::clamp(a_value, kContributionMinEV, kContributionMaxEV) : 0.0f;
	};
	occluderDepth = std::isfinite(configuration.OccluderDepth) ? std::clamp(configuration.OccluderDepth, kMinimumOccluderDepth, kMaximumOccluderDepth) : kDefaultOccluderDepth;
	rgiContributionEV = sanitizeEV(configuration.ContributionEV);
	specularContributionEV = sanitizeEV(configuration.SpecularContributionEV);
	diffuseContributionEV = sanitizeEV(configuration.DiffuseContributionEV);
	diffuseSlices = std::clamp(configuration.DiffuseSlices, 1u, 10u);
	diffuseSteps = std::clamp(configuration.DiffuseSteps, 1u, 20u);
	specularSlices = std::clamp(configuration.SpecularSlices, 1u, 10u);
	specularSteps = std::clamp(configuration.SpecularSteps, 1u, 20u);
	const float compensationDistance = a_json.value("ILCompensationDistance", a_json.value("AORadius", configuration.ILCompensationDistance));
	giCompensationMaxDistance = std::isfinite(compensationDistance) ? std::clamp(compensationDistance, 10.0f, kILRadiusMax) : 256.0f;
	specularRoughnessFloor = std::isfinite(configuration.SpecularRoughnessFloor) ? std::clamp(configuration.SpecularRoughnessFloor, kMinimumRoughnessFloor, 1.0f) : 0.2f;
	occlusion.diffuseEnabled = configuration.DiffuseOcclusionEnabled;
	occlusion.specularEnabled = configuration.SpecularOcclusionEnabled;
	occlusion.diffuseSlices = std::clamp(configuration.DiffuseOcclusionSlices, 1u, 10u);
	occlusion.specularSlices = std::clamp(configuration.SpecularOcclusionSlices, 1u, 10u);
	occlusion.diffuseSteps = std::clamp(configuration.DiffuseOcclusionSteps, 1u, 20u);
	occlusion.specularSteps = std::clamp(configuration.SpecularOcclusionSteps, 1u, 20u);
	occlusion.diffuseRadius = std::isfinite(configuration.DiffuseOcclusionRadius) ? std::clamp(configuration.DiffuseOcclusionRadius, 16.0f, 8192.0f) : 256.0f;
	occlusion.specularRadius = std::isfinite(configuration.SpecularOcclusionRadius) ? std::clamp(configuration.SpecularOcclusionRadius, 16.0f, 8192.0f) : 256.0f;
	occlusion.diffuseStrength = std::isfinite(configuration.DiffuseOcclusionStrength) ? std::clamp(configuration.DiffuseOcclusionStrength, 0.0f, 1.0f) : 1.0f;
	occlusion.specularStrength = std::isfinite(configuration.SpecularOcclusionStrength) ? std::clamp(configuration.SpecularOcclusionStrength, 0.0f, 1.0f) : 1.0f;
	rrEnabled = configuration.RREnabled;
	diffuseEnabled = configuration.DiffuseEnabled;
	specularEnabled = configuration.SpecularEnabled;
	SetRRCheckerboard(configuration.RRCheckerboard);
	denoiser.SetConfiguration(a_json.value("RRConfiguration", json::object()));
	EnforceFixedPath();
	recompileFlag = true;
	QueueHistoryReset();
}

void RedstoneGI::SaveRedstoneSettings(json& a_json) const
{
	RedstoneConfiguration configuration;
	static_cast<RedstoneGI::Settings&>(configuration) = settings;
	configuration.RREnabled = rrEnabled;
	configuration.DiffuseEnabled = diffuseEnabled;
	configuration.SpecularEnabled = specularEnabled;
	configuration.RRCheckerboard = rrCheckerboardEnabled;
	configuration.ContributionEV = rgiContributionEV;
	configuration.SpecularContributionEV = specularContributionEV;
	configuration.DiffuseContributionEV = diffuseContributionEV;
	configuration.DiffuseSlices = diffuseSlices;
	configuration.DiffuseSteps = diffuseSteps;
	configuration.SpecularSlices = specularSlices;
	configuration.SpecularSteps = specularSteps;
	configuration.OccluderDepth = occluderDepth;
	configuration.ILCompensationDistance = giCompensationMaxDistance;
	configuration.SpecularRoughnessFloor = specularRoughnessFloor;
	configuration.DiffuseOcclusionEnabled = occlusion.diffuseEnabled;
	configuration.SpecularOcclusionEnabled = occlusion.specularEnabled;
	configuration.DiffuseOcclusionSlices = occlusion.diffuseSlices;
	configuration.SpecularOcclusionSlices = occlusion.specularSlices;
	configuration.DiffuseOcclusionSteps = occlusion.diffuseSteps;
	configuration.SpecularOcclusionSteps = occlusion.specularSteps;
	configuration.DiffuseOcclusionRadius = occlusion.diffuseRadius;
	configuration.SpecularOcclusionRadius = occlusion.specularRadius;
	configuration.DiffuseOcclusionStrength = occlusion.diffuseStrength;
	configuration.SpecularOcclusionStrength = occlusion.specularStrength;
	a_json = configuration;
	a_json["RRConfiguration"] = denoiser.GetConfiguration().at("overrides");
}

void RedstoneGI::RestoreDefaultSettings()
{
	selected = false;
	settings = RedstoneConfiguration{};
	rgiContributionEV = specularContributionEV = diffuseContributionEV = 0.0f;
	diffuseSlices = specularSlices = 2;
	diffuseSteps = specularSteps = 10;
	occluderDepth = kDefaultOccluderDepth;
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
	selected = a_json.value("Selected", false);
}

void RedstoneGI::SaveSettings(json& a_json)
{
	SaveRedstoneSettings(a_json);
	a_json["Selected"] = selected;
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
	rrHistoryValid = false;
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
	DrawRendererSettings();
	DrawRayRegenerationSettings();
	ImGui::SeparatorText("RedstoneGI");
	ImGui::SliderFloat(T("feature.upscaling.redstone.contribution", "RGI Contribution (EV)"), &rgiContributionEV, kContributionMinEV, kContributionMaxEV, "%.1f", ImGuiSliderFlags_AlwaysClamp);
	recompileFlag |= ImGui::Checkbox(T("feature.upscaling.redstone.adaptive", "Adaptive Sampling"), &settings.EnableAdaptiveSampling);

	if (ImGui::SliderFloat("Occluder depth", &occluderDepth, kMinimumOccluderDepth, kMaximumOccluderDepth, "%.1f units", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)) QueueHistoryReset();
	ImGui::TextWrapped("Assumed depth behind visible surfaces; lower values can reduce silhouette halos. Applies to IL and AO.");
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

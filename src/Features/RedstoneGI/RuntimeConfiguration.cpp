#include "RuntimeDenoiser.h"
#include "Utils/RayRegenerationSettings.h"
#include "Utils/UI.h"

namespace
{
	struct ConfigurationKey
	{
		const char* name;
		const char* label;
		uint64_t key;
		float minimum;
		float maximum;
	};
	constexpr std::array kConfigurationKeys{
		ConfigurationKey{ "normalStrength", "Cross-bilateral normal strength", FFX_API_CONFIGURE_DENOISER_KEY_CROSS_BILATERAL_NORMAL_STRENGTH, 0, 1 },
		ConfigurationKey{ "stabilityBias", "Stability bias", FFX_API_CONFIGURE_DENOISER_KEY_STABILITY_BIAS, 0, 1 },
		ConfigurationKey{ "maxRadiance", "Maximum radiance", FFX_API_CONFIGURE_DENOISER_KEY_MAX_RADIANCE, 0, 65504 },
		ConfigurationKey{ "radianceClipStdK", "Radiance clipping standard deviation", FFX_API_CONFIGURE_DENOISER_KEY_RADIANCE_CLIP_STD_K, 0, 65504 },
		ConfigurationKey{ "gaussianRelaxation", "Gaussian kernel relaxation", FFX_API_CONFIGURE_DENOISER_KEY_GAUSSIAN_KERNEL_RELAXATION, 0, 1 },
		ConfigurationKey{ "disocclusionThreshold", "Disocclusion threshold", FFX_API_CONFIGURE_DENOISER_KEY_DISOCCLUSION_THRESHOLD, 0.01f, 0.05f }
	};
	constexpr std::array kBoundsNames{ "debugDepthMin", "debugDepthMax" };
	constexpr uint64_t kDebugDepthKey = FFX_API_CONFIGURE_DENOISER_KEY_DEBUG_VIEW_LINEAR_DEPTH_BOUNDS;
}

json RedstoneDenoiser::GetConfiguration() const
{
	std::scoped_lock lock(configurationMutex);
	const auto& shared = RayRegeneration::GetConfiguration();
	json overrides = json::object(), defaults = json::object();
	for (size_t i = 0; i < configurationOverrides.size(); ++i) {
		const char* name = i < kConfigurationKeys.size() ? kConfigurationKeys[i].name : kBoundsNames[i - kConfigurationKeys.size()];
		overrides[name] = shared.overrides.contains(name) ? shared.overrides.at(name) : json(nullptr);
		defaults[name] = configurationDefaultsReady ? json(configurationDefaults[i]) : json(nullptr);
	}
	return { { "overrides", overrides }, { "defaults", defaults }, { "defaultsReady", configurationDefaultsReady }, { "pending", configurationDirty || sharedConfigurationRevision != shared.revision } };
}

void RedstoneDenoiser::SetConfiguration(const json& a_configuration)
{
	if (!a_configuration.is_object()) throw std::invalid_argument("RR configuration must be an object");
	std::array<std::optional<float>, 8> selected{};
	for (size_t i = 0; i < selected.size(); ++i) {
		const char* name = i < kConfigurationKeys.size() ? kConfigurationKeys[i].name : kBoundsNames[i - kConfigurationKeys.size()];
		if (!a_configuration.contains(name) || a_configuration.at(name).is_null()) continue;
		const float value = a_configuration.at(name).get<float>();
		if (!std::isfinite(value)) throw std::invalid_argument("RR configuration values must be finite");
		if (i < kConfigurationKeys.size() && (value < kConfigurationKeys[i].minimum || value > kConfigurationKeys[i].maximum))
			throw std::invalid_argument("RR override is outside the AMD sample UI range");
		selected[i] = value;
	}
	if ((selected[6] && *selected[6] < 0) || (selected[7] && *selected[7] < 0) ||
		(selected[6] && selected[7] && *selected[6] > *selected[7]))
		throw std::invalid_argument("RR debug depth bounds must be nonnegative and ordered");
	RayRegeneration::SetConfiguration(a_configuration);
	std::scoped_lock lock(configurationMutex);
	if (selected == configurationOverrides) return;
	configurationOverrides = selected;
	configurationDirty = true;
}

bool RedstoneDenoiser::ApplyConfiguration(bool& a_resetHistory)
{
	std::scoped_lock lock(configurationMutex);
	if (!configurationDirty) return true;
	if (!configurationDefaultsReady) {
		for (size_t i = 0; i <= kConfigurationKeys.size(); ++i) {
			ffxQueryDescDenoiserGetDefaultKeyValue query{};
			query.header.type = FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE;
			query.key = i < kConfigurationKeys.size() ? kConfigurationKeys[i].key : kDebugDepthKey;
			query.count = 1;
			FfxApiFloatBounds bounds{};
			query.data = i < kConfigurationKeys.size() ? static_cast<void*>(&configurationDefaults[i]) : static_cast<void*>(&bounds);
			const auto result = denoiserFunctions.Query(&rayRegenerationContext, &query.header);
			if (result != FFX_API_RETURN_OK) {
				logger::error("[RayRegeneration] Default query failed for key {}: {}; disabling RR", query.key, result);
				rayRegenerationFailureLatched = true;
				return false;
			}
			if (i == kConfigurationKeys.size()) {
				configurationDefaults[6] = bounds.min;
				configurationDefaults[7] = bounds.max;
			}
		}
		if (!std::ranges::all_of(configurationDefaults, [](float value) { return std::isfinite(value); })) {
			logger::error("[RayRegeneration] Provider returned nonfinite configuration defaults; disabling RR");
			rayRegenerationFailureLatched = true;
			return false;
		}
		configurationDefaultsReady = true;
	}
	FfxApiFloatBounds bounds{ configurationOverrides[6].value_or(configurationDefaults[6]), configurationOverrides[7].value_or(configurationDefaults[7]) };
	if (bounds.min < 0 || bounds.max < bounds.min) {
		logger::error("[RayRegeneration] Invalid configured debug depth bounds; disabling RR");
		rayRegenerationFailureLatched = true;
		return false;
	}
	for (size_t i = 0; i <= kConfigurationKeys.size(); ++i) {
		ffxConfigureDescDenoiserKeyValue configure{};
		configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE;
		configure.key = i < kConfigurationKeys.size() ? kConfigurationKeys[i].key : kDebugDepthKey;
		configure.count = 1;
		const float value = i < kConfigurationKeys.size() ? configurationOverrides[i].value_or(configurationDefaults[i]) : 0;
		configure.data = i < kConfigurationKeys.size() ? static_cast<const void*>(&value) : static_cast<const void*>(&bounds);
		const auto result = denoiserFunctions.Configure(&rayRegenerationContext, &configure.header);
		if (result != FFX_API_RETURN_OK) {
			logger::error("[RayRegeneration] Configuration failed for key {}: {}; disabling RR", configure.key, result);
			rayRegenerationFailureLatched = true;
			return false;
		}
	}
	configurationDirty = false;
	a_resetHistory = true;
	return true;
}

void RedstoneDenoiser::DrawConfigurationSettings()
{
	SetConfiguration(RayRegeneration::GetConfiguration().overrides);
	if (!ImGui::TreeNode("RR Denoiser Configuration")) return;
	auto snapshot = GetConfiguration();
	auto selected = snapshot.at("overrides");
	const auto& defaults = snapshot.at("defaults");
	const bool ready = snapshot.at("defaultsReady").get<bool>();
	bool changed = false;
	if (!ready) ImGui::TextUnformatted("Provider defaults become available after the first RR frame.");
	for (size_t i = 0; i < configurationOverrides.size(); ++i) {
		const char* name = i < kConfigurationKeys.size() ? kConfigurationKeys[i].name : kBoundsNames[i - kConfigurationKeys.size()];
		const char* label = i < kConfigurationKeys.size() ? kConfigurationKeys[i].label : i == 6 ? "Debug depth minimum" : "Debug depth maximum";
		ImGui::PushID(name);
		bool overridden = !selected.at(name).is_null();
		ImGui::BeginDisabled(!ready);
		if (ImGui::Checkbox("Override", &overridden)) {
			selected[name] = overridden ? defaults.at(name) : json(nullptr);
			changed = true;
		}
		ImGui::SameLine();
		float value = overridden ? selected.at(name).get<float>() : ready ? defaults.at(name).get<float>() : 0;
		ImGui::BeginDisabled(!overridden);
		const bool edited = i < kConfigurationKeys.size() ?
			ImGui::SliderFloat(label, &value, kConfigurationKeys[i].minimum, kConfigurationKeys[i].maximum, "%.6g", ImGuiSliderFlags_AlwaysClamp) :
			ImGui::InputFloat(label, &value, 0, 0, "%.6g");
		if (edited && std::isfinite(value)) {
			selected[name] = value;
			changed = true;
		}
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		ImGui::PopID();
	}
	ImGui::TextWrapped("Stability bias trades responsiveness for stability. Debug depth bounds only affect the SDK depth visualization.");
	if (ImGui::Button("Restore provider defaults")) { selected = json::object(); changed = true; }
	if (changed) {
		try { SetConfiguration(selected); }
		catch (const std::exception& e) { logger::warn("[RayRegeneration] Configuration edit rejected: {}", e.what()); }
	}
	ImGui::TreePop();
}

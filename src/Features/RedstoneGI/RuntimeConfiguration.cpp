#include "RuntimeDenoiser.h"
#include "Utils/RayRegenerationSettings.h"
#include "Utils/UI.h"
#include <algorithm>

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
    constexpr std::array familyNames{ "diffuse", "specular", "ao" };
    json families = json::object();
    // Prefer the native diffuse context as the UI's reference whenever
    // it exists; still expose actual per-family defaults independently.
    const bool nativeDefaultsSelected = nativeHybridConfigurationDefaultsReady[0];
    const bool defaultsReady = nativeDefaultsSelected || configurationDefaultsReady;
    const char* source = nativeDefaultsSelected ? "native-diffuse" :
        configurationDefaultsReady ? "legacy" : "unavailable";
    for (size_t family = 0; family < nativeHybridContexts.size(); ++family) {
        json familyDefaults = json::object();
        for (size_t i = 0; i < configurationOverrides.size(); ++i) {
            const char* name = i < kConfigurationKeys.size() ?
                kConfigurationKeys[i].name : kBoundsNames[i - kConfigurationKeys.size()];
            familyDefaults[name] = nativeHybridConfigurationDefaultsReady[family] ?
                json(nativeHybridConfigurationDefaults[family][i]) : json(nullptr);
        }
        families[familyNames[family]] = {
            { "contextCreated", nativeHybridContexts[family] != nullptr },
            { "defaultsReady", nativeHybridConfigurationDefaultsReady[family] },
            { "configured", nativeHybridConfigurationApplied[family] &&
                nativeHybridAppliedConfigurationRevision == shared.revision },
            { "defaults", std::move(familyDefaults) }
        };
    }
    for (size_t i = 0; i < configurationOverrides.size(); ++i) {
        const char* name = i < kConfigurationKeys.size() ?
            kConfigurationKeys[i].name : kBoundsNames[i - kConfigurationKeys.size()];
        overrides[name] = shared.overrides.contains(name) ?
            shared.overrides.at(name) : json(nullptr);
        defaults[name] = defaultsReady ?
            json(nativeDefaultsSelected ? nativeHybridConfigurationDefaults[0][i] :
                configurationDefaults[i]) : json(nullptr);
    }
    const bool nativePending = nativeHybridContexts[0] &&
        (nativeHybridAppliedConfigurationRevision != shared.revision ||
         !std::all_of(nativeHybridConfigurationApplied.begin(),
            nativeHybridConfigurationApplied.begin() + nativeHybridActiveFamilies,
            [](bool configured) { return configured; }));
    // The legacy bridge may not be instantiated at all while testing the
    // native Hybrid path. Do not report its unapplied dirty bit as though
    // the three native contexts were still waiting for configuration.
    const bool legacyPending = rayRegenerationContext &&
        (configurationDirty || sharedConfigurationRevision != shared.revision);
    return {
        { "overrides", overrides },
        { "defaults", defaults },
        { "defaultsReady", defaultsReady },
        { "defaultsSource", source },
        { "pending", bool(legacyPending || nativePending) },
        { "legacyPending", legacyPending },
        { "native", {
            { "families", families },
            { "appliedRevision", nativeHybridAppliedConfigurationRevision == UINT64_MAX ?
                json(nullptr) : json(nativeHybridAppliedConfigurationRevision) },
            { "requestedRevision", shared.revision },
            { "pending", bool(nativePending) },
            { "activeFamilyCount", nativeHybridActiveFamilies },
            { "configurationApplications", nativeHybridConfigurationApplications },
            { "failure", nativeHybridConfigurationFailure }
        } }
    };
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

/** @brief Applies AMD's own queried defaults and shared overrides to each
 * native DX12 RR context, without touching the legacy DX11 RR context.
 * Family 0 = indirect diffuse, 1 = indirect specular, 2 = both AO signals. */
bool RedstoneDenoiser::ApplyNativeHybridConfiguration(bool& a_resetHistory)
{
    std::scoped_lock lock(configurationMutex);
    const auto& shared = RayRegeneration::GetConfiguration();
    const bool revisionChanged =
        nativeHybridAppliedConfigurationRevision != shared.revision;
    if (!revisionChanged &&
        std::all_of(nativeHybridConfigurationApplied.begin(),
            nativeHybridConfigurationApplied.begin() + nativeHybridActiveFamilies,
            [](bool applied) { return applied; }))
        return true;

    if (!denoiserFunctions.Query || !denoiserFunctions.Configure) {
        nativeHybridConfigurationFailure = "FidelityFX RR provider Query/Configure unavailable";
        return false;
    }

    constexpr std::array familyNames{ "indirect diffuse", "indirect specular", "AO" };
    for (size_t family = 0; family < nativeHybridActiveFamilies; ++family) {
        auto* context = &nativeHybridContexts[family];
        if (!*context) {
            // Missing SDK family contexts are handled by native reconstruction.
            // Do not reject configuration of other independently usable families.
            nativeHybridConfigurationApplied[family] = false;
            continue;
        }

        auto& defaults = nativeHybridConfigurationDefaults[family];
        if (!nativeHybridConfigurationDefaultsReady[family]) {
            // These values are returned by THIS native SDK context. The
            // legacy context's defaults are never assumed to be identical.
            for (size_t i = 0; i <= kConfigurationKeys.size(); ++i) {
                ffxQueryDescDenoiserGetDefaultKeyValue query{};
                query.header.type = FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE;
                query.key = i < kConfigurationKeys.size() ?
                    kConfigurationKeys[i].key : kDebugDepthKey;
                query.count = 1;
                FfxApiFloatBounds bounds{};
                query.data = i < kConfigurationKeys.size() ?
                    static_cast<void*>(&defaults[i]) : static_cast<void*>(&bounds);
                const auto result = denoiserFunctions.Query(context, &query.header);
                if (result != FFX_API_RETURN_OK) {
                    nativeHybridConfigurationFailure =
                        std::string("Native RR default query failed for ") + familyNames[family] +
                        " key " + std::to_string(query.key) + " (SDK result " +
                        std::to_string(static_cast<int>(result)) + ")";
                    logger::error("[RayRegeneration] {}", nativeHybridConfigurationFailure);
                    return false;
                }
                if (i == kConfigurationKeys.size()) {
                    defaults[6] = bounds.min;
                    defaults[7] = bounds.max;
                }
            }
            if (!std::ranges::all_of(defaults, [](float value) { return std::isfinite(value); }) ||
                defaults[6] < 0.0f || defaults[7] < defaults[6]) {
                nativeHybridConfigurationFailure =
                    std::string("Native RR provider returned invalid defaults for ") + familyNames[family];
                logger::error("[RayRegeneration] {}", nativeHybridConfigurationFailure);
                return false;
            }
            nativeHybridConfigurationDefaultsReady[family] = true;
        }

        if (!revisionChanged && nativeHybridConfigurationApplied[family]) continue;
        const FfxApiFloatBounds bounds{
            configurationOverrides[6].value_or(defaults[6]),
            configurationOverrides[7].value_or(defaults[7])
        };
        if (!std::isfinite(bounds.min) || !std::isfinite(bounds.max) ||
            bounds.min < 0.0f || bounds.max < bounds.min) {
            nativeHybridConfigurationFailure =
                std::string("Invalid configured native RR debug depth bounds for ") + familyNames[family];
            return false;
        }

        for (size_t i = 0; i <= kConfigurationKeys.size(); ++i) {
            ffxConfigureDescDenoiserKeyValue configure{};
            configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE;
            configure.key = i < kConfigurationKeys.size() ?
                kConfigurationKeys[i].key : kDebugDepthKey;
            configure.count = 1;
            const float value = i < kConfigurationKeys.size() ?
                configurationOverrides[i].value_or(defaults[i]) : 0.0f;
            configure.data = i < kConfigurationKeys.size() ?
                static_cast<const void*>(&value) : static_cast<const void*>(&bounds);
            const auto result = denoiserFunctions.Configure(context, &configure.header);
            if (result != FFX_API_RETURN_OK) {
                nativeHybridConfigurationApplied[family] = false;
                nativeHybridConfigurationFailure =
                    std::string("Native RR Configure failed for ") + familyNames[family] +
                    " key " + std::to_string(configure.key) + " (SDK result " +
                    std::to_string(static_cast<int>(result)) + ")";
                logger::error("[RayRegeneration] {}", nativeHybridConfigurationFailure);
                return false;
            }
        }
        nativeHybridConfigurationApplied[family] = true;
        ++nativeHybridConfigurationApplications;
    }

    nativeHybridAppliedConfigurationRevision = shared.revision;
    nativeHybridConfigurationFailure.clear();
    a_resetHistory = true; // SDK parameters changed: reset all affected histories
    logger::info("[RayRegeneration] Native RR configuration revision {} applied to {} active families",
        nativeHybridAppliedConfigurationRevision, nativeHybridActiveFamilies);
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
    const auto& native = snapshot.at("native");
    const auto& families = native.at("families");
    bool changed = false;
    if (!ready)
        ImGui::TextUnformatted("Provider defaults appear after a native or legacy RR context initializes.");
    else
        ImGui::Text("AMD defaults source: %s", snapshot.at("defaultsSource").get<std::string>().c_str());
    ImGui::Text("Native RR configuration applications: %llu",
        static_cast<unsigned long long>(native.value("configurationApplications", uint64_t{0})));
    const auto applied = native.at("appliedRevision");
    if (!applied.is_null())
        ImGui::Text("Native applied revision: %llu | requested: %llu",
            static_cast<unsigned long long>(applied.get<uint64_t>()),
            static_cast<unsigned long long>(native.value("requestedRevision", uint64_t{0})));
    if (native.at("pending").get<bool>())
        ImGui::TextUnformatted("Native configuration changes pending SDK application.");
    ImGui::Text("Native RR context families: %u",
        native.value("activeFamilyCount", 3u));
    const auto failure = native.value("failure", std::string{});
    if (!failure.empty()) ImGui::TextWrapped("Native RR configure failure: %s", failure.c_str());
    ImGui::TextWrapped("All three native contexts receive the shared overrides. With Override off, each context uses its own queried AMD defaults.");
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
        if (ready) {
            const float providerDefault = defaults.at(name).get<float>();
            ImGui::Text("SDK reference default: %.6g | selected: %.6g",
                providerDefault, overridden ? selected.at(name).get<float>() : providerDefault);
            if (families.at("diffuse").at("defaultsReady").get<bool>() &&
                families.at("specular").at("defaultsReady").get<bool>() &&
                families.at("ao").at("defaultsReady").get<bool>()) {
                const float diffuse = families.at("diffuse").at("defaults").at(name).get<float>();
                const float specular = families.at("specular").at("defaults").at(name).get<float>();
                const float ao = families.at("ao").at("defaults").at(name).get<float>();
                ImGui::TextDisabled("Native defaults: diffuse %.6g | specular %.6g | AO %.6g",
                    diffuse, specular, ao);
            }
        }
        ImGui::PopID();
	}
    ImGui::TextWrapped("Stability bias trades responsiveness for stability. Debug depth bounds only affect the SDK depth visualization. Values are provider-reported and configured directly into the native DX12 contexts, not applied as a separate shader multiplier.");
	if (ImGui::Button("Restore provider defaults")) { selected = json::object(); changed = true; }
	if (changed) {
		try { SetConfiguration(selected); }
		catch (const std::exception& e) { logger::warn("[RayRegeneration] Configuration edit rejected: {}", e.what()); }
	}
	ImGui::TreePop();
}

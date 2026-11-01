#include "RTComposer.h"

#include "Features/DX12NativeScene/DX12NativeScene.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Features/RTDirectLighting/RTDirectLighting.h"
#include "Features/RTIndirectLighting/RTIndirectLighting.h"
#include "Features/ScreenSpaceGI.h"
#include "Globals.h"
#include "Utils/DevBenchUx.h"
#include "Utils/RayRegenerationSettings.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    constexpr std::array kDirectKeys{ "UseDirectRayRegeneration", "DirectRRResolution", "RRMaterialIsolation", "RRGlossyRoughness" };
    constexpr std::array kHybridFamilies{ "diffuse", "specular", "ao" };
    using HybridFilters = std::array<RTComposer::SignalReconstructionSettings, 3>;
    json SaveHybridFilters(const HybridFilters& filters)
    {
        json result = json::object();
        for (size_t i = 0; i < filters.size(); ++i) {
            const auto& s = filters[i];
            result[kHybridFamilies[i]] = {
                { "filterEnabled", s.filterEnabled },
                { "filterRadius", s.filterRadius }, { "filterStrength", s.filterStrength },
                { "depthSensitivity", s.depthSensitivity },
                { "normalSensitivity", s.normalSensitivity },
                { "roughnessSensitivity", s.roughnessSensitivity },
                { "blurEnabled", s.blurEnabled },
                { "blurRadius", s.blurRadius }, { "blurStrength", s.blurStrength }
            };
        }
        return result;
    }
    void LoadHybridFilters(const json& from, HybridFilters& filters)
    {
        if (!from.is_object()) return;
        for (size_t i = 0; i < filters.size(); ++i) {
            auto it = from.find(kHybridFamilies[i]);
            if (it == from.end() || !it->is_object()) continue;
            auto& s = filters[i];
            auto bounded = [&](const char* key, float fallback, float low, float high) {
                const float x = it->value(key, fallback);
                return std::isfinite(x) ? std::clamp(x, low, high) : fallback;
            };
            s.filterEnabled = it->value("filterEnabled", s.filterEnabled);
            s.filterRadius = bounded("filterRadius", s.filterRadius, 0.5f, 4.0f);
            s.filterStrength = bounded("filterStrength", s.filterStrength, 0.0f, 1.0f);
            s.depthSensitivity = bounded("depthSensitivity", s.depthSensitivity, 1.0f, 256.0f);
            s.normalSensitivity = bounded("normalSensitivity", s.normalSensitivity, 1.0f, 128.0f);
            s.roughnessSensitivity = bounded("roughnessSensitivity", s.roughnessSensitivity, 0.0f, 64.0f);
            s.blurEnabled = it->value("blurEnabled", s.blurEnabled);
            s.blurRadius = bounded("blurRadius", s.blurRadius, 0.5f, 3.0f);
            s.blurStrength = bounded("blurStrength", s.blurStrength, 0.0f, 1.0f);
        }
    }

    void CopyMissing(json& destination, const json& source, const char* field)
    {
        if (source.is_object() && source.contains(field) && !destination.contains(field))
            destination[field] = source[field];
    }
}

std::pair<std::string, std::vector<std::string>> RTComposer::GetFeatureSummary()
{
    return { "Central Ray Regeneration and mutually exclusive RT shadow composition; Shadow Only permits baseline SSGI, never RT GI.", {} };
}
void RTComposer::OnCompositionPolicyChanged()
{
    // Saved RT-exclusive/Hybrid settings are not mutated. Incompatible GPU
    // history is discarded when the effective compositor selection changes.
    globals::features::redstoneGI.ResetRuntimeHistory();
    globals::features::screenSpaceGI.QueueHistoryReset();
    globals::features::dx12NativeScene.Tracer().Reset();
}
json RTComposer::GetDiagnostics()
{
    const auto effective = ResolveEffectiveMode();
    const bool ssgiAvailable = globals::features::screenSpaceGI.loaded &&
        globals::features::screenSpaceGI.settings.Enabled;
    const auto& tracer = globals::features::dx12NativeScene.Tracer();
    return {
        { "enabled", composition.enabled },
        { "requestedMode", composition.mode == CompositorMode::ShadowOnly ? "shadow-only" : "hybrid-shadowed" },
        { "effectiveMode", effective == EffectiveMode::Legacy ? "legacy" :
            effective == EffectiveMode::ShadowOnly ? "shadow-only" : "hybrid-shadowed" },
        { "hybridDX12Ready", DX12HybridReady() },
        { "hybridFallbackReason", composition.enabled &&
            composition.mode == CompositorMode::HybridShadowed && !DX12HybridReady() ?
            "DX12 HybridGI unavailable; Shadow Only fallback selected" : "" },
        { "indirectRestriction", BlocksRTIndirect() ? "disabled-by-compositor-policy" : "" },
        { "rtExclusiveAllowed", !BlocksRTIndirect() },
        { "dx11HybridAllowed", !BlocksRTIndirect() },
        { "ssgiAvailable", ssgiAvailable },
        { "giProviderStatus", effective != EffectiveMode::Legacy ?
            (ssgiAvailable && effective == EffectiveMode::ShadowOnly ? "baseline-ssgi" :
             effective == EffectiveMode::ShadowOnly ? "no-active-indirect-provider" : "dx12-hybrid") : "legacy" },
        { "shadowInfluence", composition.shadowInfluence },
        { "directContribution", composition.rtDirectContribution },
        { "giContribution", composition.giContribution },
        { "nativeRawPreviewRequested", composition.previewNativeRaw },
        { "hybridSamplingMode", static_cast<uint32_t>(composition.samplingMode) },
        { "shadowThreshold", composition.shadowThreshold },
        { "rtIndirectTraceDispatchesCumulative", tracer.GetDiagnostics().value("indirectTraceDispatches", uint64_t{ 0 }) }
    };
}

void RTComposer::MigrateLegacySettings(json& root)
{
    if (!root.is_object()) return;
    auto& composer = root["RTComposer"];
    if (!composer.is_object()) composer = json::object();

    json oldRR = json::object();
    if (auto up = root.find("Upscaling"); up != root.end() && up->is_object()) {
        if (auto rr = up->find("FSRRayRegeneration"); rr != up->end() && rr->is_object()) oldRR = *rr;
    }
    const json* oldGI = nullptr;
    if (auto gi = root.find("RedstoneGI"); gi != root.end() && gi->is_object()) oldGI = &*gi;
    else if (auto up = root.find("Upscaling"); up != root.end() && up->is_object()) {
        if (auto legacyGI = up->find("redstoneGI"); legacyGI != up->end() && legacyGI->is_object()) oldGI = &*legacyGI;
    }
    if (oldGI) {
        if (!oldRR.contains("resolutionMode") && oldGI->contains("ResolutionMode"))
            oldRR["resolutionMode"] = (*oldGI)["ResolutionMode"];
        if (!oldRR.contains("checkerboard") && oldGI->contains("RRCheckerboard"))
            oldRR["checkerboard"] = (*oldGI)["RRCheckerboard"];
        if (!oldRR.contains("enabled") && oldGI->contains("RREnabled"))
            oldRR["enabled"] = (*oldGI)["RREnabled"];
        if (!oldRR.contains("configuration") && oldGI->contains("RRConfiguration"))
            oldRR["configuration"] = (*oldGI)["RRConfiguration"];
    }
    if (!oldRR.empty()) {
        auto& target = composer["FSRRayRegeneration"];
        if (!target.is_object()) target = json::object();
        for (const auto& [key, v] : oldRR.items()) if (!target.contains(key)) target[key] = v;
    }
    json oldDirect = json::object();
    if (auto older = root.find("RedstoneShadows"); older != root.end() && older->is_object())
        for (const char* key : kDirectKeys) CopyMissing(oldDirect, *older, key);
    if (auto direct = root.find("RTDirectLighting"); direct != root.end() && direct->is_object())
        for (const char* key : kDirectKeys) if (direct->contains(key)) oldDirect[key] = (*direct)[key];
    if (!oldDirect.empty()) {
        auto& target = composer["DirectRR"];
        if (!target.is_object()) target = json::object();
        for (const auto& [key, v] : oldDirect.items()) if (!target.contains(key)) target[key] = v;
    }
}

void RTComposer::MigrateChangedLegacySettings(const json& before, json& after)
{
    if (!after.is_object() || !before.is_object()) return;
    // Only migrate keys introduced or changed by the later, higher-priority layer.
    json sparse = json::object();
    auto addChanged = [&](const char* owner, const char* nested, const char* key) {
        auto nowOwner = after.find(owner);
        if (nowOwner == after.end() || !nowOwner->is_object()) return;
        const json* now = &*nowOwner;
        const json* old = nullptr;
        if (auto prev = before.find(owner); prev != before.end() && prev->is_object()) old = &*prev;
        if (nested) {
            auto nowNested = now->find(nested);
            if (nowNested == now->end() || !nowNested->is_object()) return;
            now = &*nowNested;
            if (old) {
                auto oldNested = old->find(nested);
                old = oldNested != old->end() && oldNested->is_object() ? &*oldNested : nullptr;
            }
        }
        auto current = now->find(key);
        if (current == now->end()) return;
        if (old && old->contains(key) && (*old)[key] == *current) return;
        if (nested) sparse[owner][nested][key] = *current;
        else sparse[owner][key] = *current;
    };
    for (const char* key : { "enabled", "checkerboard", "resolutionMode", "configuration" })
        addChanged("Upscaling", "FSRRayRegeneration", key);
    for (const char* key : kDirectKeys) {
        addChanged("RTDirectLighting", nullptr, key);
        addChanged("RedstoneShadows", nullptr, key);
    }
    for (const char* key : { "ResolutionMode", "RRCheckerboard", "RREnabled", "RRConfiguration" })
        addChanged("RedstoneGI", nullptr, key);
    if (!sparse.empty()) {
        MigrateLegacySettings(sparse);
        if (sparse.contains("RTComposer")) after["RTComposer"].merge_patch(sparse["RTComposer"]);
    }
}

void RTComposer::ImportLegacySettings(json& root)
{
    MigrateLegacySettings(root);
}

void RTComposer::SaveSettings(json& value)
{
    const auto policy = RayRegeneration::GetSettings();
    const auto& direct = globals::features::rtDirectLighting.settings;
    value = {
        { "FSRRayRegeneration", {
            { "enabled", policy.enabled },
            { "checkerboard", policy.checkerboard },
            { "resolutionMode", policy.resolutionMode },
            { "configuration", RayRegeneration::GetConfiguration().overrides }
        } },
        { "DirectRR", {
            { "UseDirectRayRegeneration", direct.UseDirectRayRegeneration },
            { "DirectRRResolution", direct.DirectRRResolution },
            { "RRMaterialIsolation", direct.RRMaterialIsolation },
            { "RRGlossyRoughness", direct.RRGlossyRoughness }
        } },
        { "Composition", {
            { "enabled", composition.enabled },
            { "mode", static_cast<uint32_t>(composition.mode) },
            { "shadowInfluence", composition.shadowInfluence },
            { "rtDirectContribution", composition.rtDirectContribution },
            { "giContribution", composition.giContribution },
            { "shadowThreshold", composition.shadowThreshold },
            { "samplingMode", static_cast<uint32_t>(composition.samplingMode) },
            { "reconstruction", SaveHybridFilters(composition.reconstruction) }
        } }
    };
}

void RTComposer::LoadSettings(json& value)
{
    if (!value.is_object()) return;
    const auto beforePolicy = std::pair{ composition.enabled, composition.mode };
    // Each setting is optional: old profiles retain the disabled legacy path.
    if (auto it = value.find("Composition"); it != value.end() && it->is_object()) {
        CompositionSettings next{};
        next.enabled = it->value("enabled", false);
        const auto mode = it->value("mode", 0u);
        next.mode = mode == 1u ? CompositorMode::HybridShadowed : CompositorMode::ShadowOnly;
        auto bounded = [&](const char* name, float fallback) {
            const float x = it->value(name, fallback);
            return std::isfinite(x) ? std::clamp(x, 0.0f, 1.0f) : fallback;
        };
        next.shadowInfluence = bounded("shadowInfluence", 1.0f);
        next.rtDirectContribution = bounded("rtDirectContribution", 0.0f);
        next.giContribution = bounded("giContribution", 1.0f);
        next.shadowThreshold = bounded("shadowThreshold", 0.25f);
        const uint32_t sampling = it->value("samplingMode", 0u);
        next.samplingMode = static_cast<DX12Native::HybridSamplingMode>(std::min(sampling, 3u));
        if (auto filters = it->find("reconstruction"); filters != it->end())
            LoadHybridFilters(*filters, next.reconstruction);
        composition = next;
    } else composition = {};
    if (beforePolicy != std::pair{ composition.enabled, composition.mode }) OnCompositionPolicyChanged();
    if (auto it = value.find("FSRRayRegeneration"); it != value.end() && it->is_object()) {
        const auto prior = RayRegeneration::GetSettings();
        RayRegeneration::Configure(it->value("resolutionMode", prior.resolutionMode),
            it->value("checkerboard", prior.checkerboard), it->value("enabled", prior.enabled));
        if (auto cfg = it->find("configuration"); cfg != it->end() && cfg->is_object())
            globals::features::redstoneGI.GetDenoiser().SetConfiguration(*cfg);
    }
    if (auto it = value.find("DirectRR"); it != value.end() && it->is_object()) {
        auto& direct = globals::features::rtDirectLighting.settings;
        const auto beforeResolution = direct.DirectRRResolution;
        const auto beforeIsolation = direct.RRMaterialIsolation;
        const auto beforeRoughness = direct.RRGlossyRoughness;
        direct.UseDirectRayRegeneration = it->value("UseDirectRayRegeneration", direct.UseDirectRayRegeneration);
        direct.DirectRRResolution = std::min(it->value("DirectRRResolution", direct.DirectRRResolution), 3u);
        direct.RRMaterialIsolation = it->value("RRMaterialIsolation", direct.RRMaterialIsolation);
        const auto roughness = it->value("RRGlossyRoughness", direct.RRGlossyRoughness);
        direct.RRGlossyRoughness = std::isfinite(roughness) ? std::clamp(roughness, 0.04f, 1.0f) : 0.2f;
        if (beforeResolution != direct.DirectRRResolution || beforeIsolation != direct.RRMaterialIsolation ||
            beforeRoughness != direct.RRGlossyRoughness)
            globals::features::dx12NativeScene.Tracer().Reset();
    }
}

void RTComposer::RestoreDefaultSettings()
{
    const auto beforePolicy = std::pair{ composition.enabled, composition.mode };
    composition = {};
    if (beforePolicy != std::pair{ composition.enabled, composition.mode }) OnCompositionPolicyChanged();
    RayRegeneration::Configure(1u, true, true);
    globals::features::redstoneGI.GetDenoiser().SetConfiguration(json::object());
    auto& direct = globals::features::rtDirectLighting.settings;
    direct.UseDirectRayRegeneration = true;
    direct.DirectRRResolution = 3u;
    direct.RRMaterialIsolation = false;
    direct.RRGlossyRoughness = 0.2f;
}

void RTComposer::DrawSettings()
{
    if (globals::game::isVR) {
        ImGui::TextUnformatted("FSR Ray Regeneration is unavailable in VR.");
        return;
    }
    ImGui::PushID("RTComposer");
    ImGui::SeparatorText("Compositor");
    const auto previousPolicy = std::pair{ composition.enabled, composition.mode };
    ImGui::Checkbox("Enable RT shadow composition", &composition.enabled);
    int compositorMode = static_cast<int>(composition.mode);
    if (ImGui::Combo("Compositor mode", &compositorMode, "Shadow Only\0Hybrid-Shadowed\0"))
        composition.mode = compositorMode == 1 ? CompositorMode::HybridShadowed : CompositorMode::ShadowOnly;
    if (previousPolicy != std::pair{ composition.enabled, composition.mode }) OnCompositionPolicyChanged();
    ImGui::SliderFloat("Shadow influence", &composition.shadowInfluence, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("RT direct contribution", &composition.rtDirectContribution, 0.0f, 1.0f, "%.2f");
    if (composition.mode == CompositorMode::HybridShadowed) {
        ImGui::SliderFloat("Native GI contribution", &composition.giContribution, 0.0f, 1.0f, "%.2f");
        int hybridSampling = static_cast<int>(composition.samplingMode);
        if (ImGui::Combo("Native hybrid sampling", &hybridSampling,
                "Native (100%)\0Native checkerboard (50%)\0Quarter (25%)\0Quarter checkerboard (12.5%)\0")) {
            composition.samplingMode = static_cast<DX12Native::HybridSamplingMode>(hybridSampling);
            OnCompositionPolicyChanged();
        }
        ImGui::TextWrapped("Quarter is one sample per 2x2 block; its checkerboard phase alternates by logical block. The native RR behavior is unchanged in this stage.");
        ImGui::SeparatorText("Post-RR spatial reconstruction (staged)");
        ImGui::TextWrapped("Saved controls for future native RR output; raw diagnostic mode does not apply these filters.");
        constexpr std::array familyNames{ "Indirect diffuse", "Indirect specular", "Ambient/specular AO" };
        for (size_t family = 0; family < composition.reconstruction.size(); ++family) {
            ImGui::PushID(static_cast<int>(family));
            auto& settings = composition.reconstruction[family];
            if (ImGui::TreeNode(familyNames[family])) {
                bool changed = false;
                changed |= ImGui::Checkbox("Edge-aware filter", &settings.filterEnabled);
                changed |= ImGui::SliderFloat("Filter radius", &settings.filterRadius, 0.5f, 4.0f, "%.1f px");
                changed |= ImGui::SliderFloat("Filter strength", &settings.filterStrength, 0.0f, 1.0f);
                changed |= ImGui::SliderFloat("Depth sensitivity", &settings.depthSensitivity, 1.0f, 256.0f);
                changed |= ImGui::SliderFloat("Normal sensitivity", &settings.normalSensitivity, 1.0f, 128.0f);
                changed |= ImGui::SliderFloat("Roughness sensitivity", &settings.roughnessSensitivity, 0.0f, 64.0f);
                changed |= ImGui::Checkbox("Post-filter blur", &settings.blurEnabled);
                changed |= ImGui::SliderFloat("Blur radius", &settings.blurRadius, 0.5f, 3.0f, "%.1f px");
                changed |= ImGui::SliderFloat("Blur strength", &settings.blurStrength, 0.0f, 1.0f);
                if (changed) OnCompositionPolicyChanged();
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (ImGui::SliderFloat("GI skip visibility threshold", &composition.shadowThreshold, 0.0f, 1.0f, "%.2f"))
            OnCompositionPolicyChanged();
        if (ImGui::Checkbox("Preview raw native Hybrid-Shadowed (diagnostic, no RR)", &composition.previewNativeRaw))
            OnCompositionPolicyChanged();
        if (composition.previewNativeRaw &&
            composition.samplingMode != DX12Native::HybridSamplingMode::Native)
            ImGui::TextWrapped("Raw preview is available only at full native density. Reduced-density samples still dispatch for diagnostics, but cannot be displayed as finished lighting before RR and post-RR reconstruction; Shadow Only stays active.");
        else
            ImGui::TextWrapped("Native GI/AO composition and GPU RR guides are diagnostic. RR provider dispatch, selective history rejection and post-RR reconstruction are still gated.");
        const auto trace = globals::features::dx12NativeScene.Tracer().GetDiagnostics();
        ImGui::Text("DX12 input bridge dispatches: %llu",
            static_cast<unsigned long long>(trace.value("hybridInputDispatches", uint64_t{ 0 })));
        ImGui::Text("DX12 shadowed-source dispatches: %llu",
            static_cast<unsigned long long>(trace.value("hybridShadowedSourceDispatches", uint64_t{ 0 })));
        ImGui::Text("Bridge memory allocation: %.1f MiB",
            double(trace.value("hybridInputLastBytes", uint64_t{ 0 })) / (1024.0 * 1024.0));
        ImGui::Text("Bridge budget deferrals: %llu",
            static_cast<unsigned long long>(trace.value("hybridInputBudgetDeferrals", uint64_t{ 0 })));
        ImGui::Text("Native GI/AO dispatches: %llu / %llu | DX12 composed scenes: %llu",
            static_cast<unsigned long long>(trace.value("nativeHybridGIDispatches", uint64_t{ 0 })),
            static_cast<unsigned long long>(trace.value("nativeHybridAODispatches", uint64_t{ 0 })),
            static_cast<unsigned long long>(trace.value("nativeHybridCompositionDispatches", uint64_t{ 0 })));
        ImGui::Text("Full-native eligibility dispatches: %llu",
            static_cast<unsigned long long>(trace.value("nativeHybridEligibilityDispatches", uint64_t{ 0 })));
        ImGui::Text("Native GPU-only RR guide dispatches: %llu",
            static_cast<unsigned long long>(trace.value("nativeHybridRRGuideDispatches", uint64_t{ 0 })));
        const auto guideFailure = trace.value("nativeHybridRRGuideFailure", std::string{});
        if (!guideFailure.empty())
            ImGui::TextWrapped("Native RR guide preparation: %s", guideFailure.c_str());
        const auto logical = trace.value("nativeHybridLogicalGrid", std::array<uint32_t, 2>{});
        const auto packed = trace.value("nativeHybridPackedGrid", std::array<uint32_t, 2>{});
        ImGui::Text("Hybrid RR grid: %u x %u | trace storage: %u x %u",
            logical[0], logical[1], packed[0], packed[1]);
        ImGui::Text("Active traced pixels: %llu | 2x2 phase: %u | checkerboard origin: %u",
            static_cast<unsigned long long>(trace.value("nativeHybridTracedPixels", uint64_t{0})),
            trace.value("nativeHybridSamplePhase", 0u),
            trace.value("nativeHybridCheckerboardOrigin", 0u));
        ImGui::Text("Native diagnostic scene exports: %llu",
            static_cast<unsigned long long>(trace.value("nativeHybridPreviewSubmissions", uint64_t{ 0 })));
        const auto giFailure = trace.value("nativeHybridGIFailure", std::string{});
        const auto previewFailure = trace.value("nativeHybridPreviewFailure", std::string{});
        if (!giFailure.empty()) ImGui::TextWrapped("Native GI/AO: %s", giFailure.c_str());
        if (!previewFailure.empty()) ImGui::TextWrapped("DX11 output handoff: %s", previewFailure.c_str());
        const auto compositionFailure = trace.value("nativeHybridCompositionFailure", std::string{});
        if (!compositionFailure.empty()) ImGui::TextWrapped("DX12 native composition: %s", compositionFailure.c_str());
        const auto inputFailure = trace.value("hybridInputFailure", std::string{});
        const auto sourceFailure = trace.value("hybridShadowedSourceFailure", std::string{});
        if (!inputFailure.empty()) ImGui::TextWrapped("DX12 bridge: %s", inputFailure.c_str());
        if (!sourceFailure.empty()) ImGui::TextWrapped("DX12 source: %s", sourceFailure.c_str());
    } else {
        ImGui::TextWrapped("Shadow Only preserves existing DX11 SSGI and masks the final raster image. Invalid visibility remains neutral.");
    }
    if (composition.enabled) {
        const bool ssgiAvailable = globals::features::screenSpaceGI.loaded && globals::features::screenSpaceGI.settings.Enabled;
        ImGui::TextWrapped("RT-exclusive GI and DX11 RedstoneGI unavailable: disabled by RT Composer mode.");
        if (!ssgiAvailable) ImGui::TextWrapped("No active indirect-lighting provider. RT shadows and optional direct radiance remain available.");
    }
    ImGui::TextWrapped("Compositor controls are opt-in. Existing lighting and RR behavior remains unchanged while disabled.");
    ImGui::SeparatorText("FSR Ray Regeneration");
    const auto old = RayRegeneration::GetSettings();
    bool enabled = old.enabled, checkerboard = old.checkerboard;
    int resolution = static_cast<int>(old.resolutionMode);
    bool changed = ImGui::Checkbox("Enable FSR Ray Regeneration", &enabled);
    changed |= ImGui::Combo("Reconstruction resolution", &resolution, "Full resolution\0Half resolution\0Quarter resolution\0");
    changed |= ImGui::Checkbox("Checkerboard RR inputs", &checkerboard);
    if (changed) RayRegeneration::Configure(uint32_t(resolution), checkerboard, enabled);
    ImGui::TextWrapped("Shared RR policy. Independent Direct, RT-exclusive and Hybrid policies remain deferred.");
    ImGui::SeparatorText("Direct Lighting RR");
    auto& direct = globals::features::rtDirectLighting.settings;
    int directResolution = int(direct.DirectRRResolution);
    if (ImGui::Combo("Direct RR resolution", &directResolution, "Full\0Half\0Quarter\0Use shared RR setting (legacy)\0")) {
        direct.DirectRRResolution = uint32_t(directResolution);
        globals::features::dx12NativeScene.Tracer().Reset();
    }
    globals::features::rtDirectLighting.DrawRayRegenerationSettings();
    ImGui::SeparatorText("RR Signals and Diagnostics");
    globals::features::rtIndirectLighting.DrawUnifiedDebugSettings();
    if (!globals::features::rtIndirectLighting.UsesRTExclusive()) {
        ImGui::SeparatorText("Hybrid GI / AO Ray Regeneration");
        globals::features::redstoneGI.DrawRayRegenerationSettings();
    }
    ImGui::PopID();
}

void RTComposer::RegisterUxActions()
{
    auto& registry = Util::DevBenchUx::Registry::GetSingleton();
    registry.RegisterCommand(GetShortName(), "setComposition",
        "Set independent shadow compositor mode and influence; Hybrid-Shadowed remains gated until DX12 GI is ready.",
        +[](Feature* self, const json& args) {
            auto& composer = *static_cast<RTComposer*>(self);
            auto& policy = composer.composition;
            const auto beforePolicy = std::pair{ policy.enabled, policy.mode };
            if (args.contains("enabled")) policy.enabled = args.at("enabled").get<bool>();
            if (args.contains("mode")) {
                const auto mode = args.at("mode").get<uint32_t>();
                if (mode > 1u) throw std::invalid_argument("Unknown RT compositor mode");
                policy.mode = mode == 1u ? CompositorMode::HybridShadowed : CompositorMode::ShadowOnly;
            }
            auto setUnit = [&](const char* key, float& target) {
                if (!args.contains(key)) return;
                const auto value = args.at(key).get<float>();
                if (!std::isfinite(value)) throw std::invalid_argument("Composition value must be finite");
                target = std::clamp(value, 0.0f, 1.0f);
            };
            setUnit("shadowInfluence", policy.shadowInfluence);
            setUnit("rtDirectContribution", policy.rtDirectContribution);
            setUnit("shadowThreshold", policy.shadowThreshold);
            if (beforePolicy != std::pair{ policy.enabled, policy.mode }) composer.OnCompositionPolicyChanged();
        });
    registry.RegisterCommand(GetShortName(), "setRayRegeneration",
        "Set shared RR controls; saved under RTComposer.FSRRayRegeneration.",
        +[](Feature*, const json& args) {
            const auto prior = RayRegeneration::GetSettings();
            RayRegeneration::Configure(args.value("resolutionMode", prior.resolutionMode),
                args.value("checkerboard", prior.checkerboard), args.value("enabled", prior.enabled));
        });
}

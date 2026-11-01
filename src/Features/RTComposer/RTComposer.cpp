#include "RTComposer.h"

#include "Features/DX12NativeScene/DX12NativeScene.h"
#include "Features/DX12NativeScene/NativeHybridGI.h"
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
#include <tuple>

namespace
{
    constexpr std::array kDirectKeys{ "UseDirectRayRegeneration", "DirectRRResolution", "RRMaterialIsolation", "RRGlossyRoughness" };
    constexpr std::array kHybridFamilies{ "diffuse", "specular", "ao" };
    void ApplySamplingPolicy(bool directCheckerboard)
    {
        // SDK-wide compatibility only serves direct RR. The native GI
        // denoiser receives its own logical dimensions and packed signals.
        const auto rr = RayRegeneration::GetSettings();
        RayRegeneration::Configure(0u, directCheckerboard, rr.enabled);
        globals::features::rtDirectLighting.settings.DirectRRResolution = 0u;
    }

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

const char* RTComposer::DirectTierDescription(CompositorMode mode)
{
    switch (mode) {
    case CompositorMode::ShadowOnly:
        return "Quarter-density (1/4 native pixels), surface-aware shadow-mask resolve; 0.8 shadow influence, 0.25 raster/RT blend. Direct diffuse only, no specular and no direct RR.";
    case CompositorMode::HybridShadowed:
        return "Quarter-density direct/shadow DXR; shadow influence 0.8, raster/RT direct blend 0.5. Direct diffuse only, direct RR off; native DX12 HybridGI handles indirect/AO RR.";
    case CompositorMode::RTExclusiveI:
        return "PENDING: quarter-density direct/shadows, diffuse only, 50% first-bounce survival, diffuse-only RR. The live renderer safely falls back to Shadow Only.";
    case CompositorMode::RTExclusiveII:
        return "PENDING: quarter-density direct/shadows, 50% first-bounce survival, 50/50 diffuse/specular branch split. The live renderer falls back to Shadow Only.";
    case CompositorMode::RTEnhanced:
        return "PENDING: native checkerboard, 50/50 diffuse/specular first-bounce split, 50% indirect survival and lowest-quality checkerboard HybridGI. The live renderer falls back to Shadow Only.";
    case CompositorMode::FSRRedstone:
        return "PENDING: native checkerboard diffuse/specular split, 66% first-bounce survival, and 25% second-bounce continuation. High-end recent AMD GPU recommended; FSR-RC is deferred.";
    case CompositorMode::Custom:
        return "Custom: editable direct, shadow, material and RR options; separate SSGI-sized HybridGI resolution.";
    }
    return "Unknown compositor tier.";
}
const char* RTComposer::IndirectTierDescription(CompositorMode mode)
{
    switch (mode) {
    case CompositorMode::ShadowOnly:
        return "Native HybridGI and RT-exclusive indirect are disabled. Baseline raster SSGI may remain active; legacy DX11 HybridGI stays disabled. No FSR-RR for direct/shadow.";
    case CompositorMode::HybridShadowed:
        return "Native DX12 HybridGI: quarter width/height by default (6.25% pixels), checkerboard off. Indirect diffuse/specular, AO and specular occlusion use RR and native DX12 reconstruction.";
    case CompositorMode::RTExclusiveI:
        return "PENDING: a diffuse-only indirect bounce from 50% surviving rays, using diffuse-only RR signals. No tier-exclusive indirect rays are currently dispatched.";
    case CompositorMode::RTExclusiveII:
        return "PENDING: 50% indirect survival, diffuse/specular split and family-specific RR mapping. No tier-exclusive indirect rays are currently dispatched.";
    case CompositorMode::RTEnhanced:
        return "PENDING: 50% indirect survival, native DX12 scene composition, then quarter width/height checkerboard HybridGI. Tier-exclusive transport is not active.";
    case CompositorMode::FSRRedstone:
        return "PENDING: 66% initial bounce survival, 25% secondary continuation and future FSR Radiance Cache. Radiance Cache remains unimplemented.";
    case CompositorMode::Custom:
        return "Custom: select Shadow Only or DX12 Hybrid-Shadowed in RT Composer; edit indirect and AO sampling, reconstruction and RR settings.";
    }
    return "Unknown compositor tier.";
}
std::pair<std::string, std::vector<std::string>> RTComposer::GetFeatureSummary()
{
    return { "Six locked compositor tiers and a persistent Custom profile, with mode-specific direct/indirect settings panels; native DX12 HybridGI and RR remain available in supported modes.", {} };
}

bool RTComposer::DX12HybridReady() const
{
    const auto& direct = globals::features::rtDirectLighting;
    const auto& native = globals::features::dx12NativeScene;
    return !globals::game::isVR && direct.loaded && direct.settings.UsePreparedDirectLighting &&
        native.loaded && native.settings.UsePreparedScene && native.settings.UsePreparedMaterials &&
        bool(native.GetPublishedScene()) && DX12Native::NativeHybridGI::ShadersAvailable();
}
void RTComposer::OnCompositionPolicyChanged()
{
    // On leaving native Hybrid-Shadowed, release AMD's private RR histories
    // after queue retirement. Do not keep three contexts consuming VRAM
    // throughout Shadow Only or when the compositor is disabled.
    if (!composition.enabled || !WantsNativeHybrid())
        (void)globals::features::redstoneGI.GetDenoiser().RetireNativeHybridContexts();
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
        { "requestedMode", ModeName(composition.mode) },
        { "requestedTierReady", composition.mode == CompositorMode::Custom || GetTierPolicy(composition.mode).ready },
        { "tierControlsLocked", TierControlsLocked() },
        { "customHybridShadowed", composition.customHybridShadowed },
        { "firstBounceSurvival", GetTierPolicy(composition.mode).firstBounceSurvival },
        { "specularBounceFraction", GetTierPolicy(composition.mode).specularBounceFraction },
        { "secondBounceSurvival", GetTierPolicy(composition.mode).secondBounceSurvival },
        { "effectiveMode", effective == EffectiveMode::Legacy ? "legacy" :
            effective == EffectiveMode::ShadowOnly ? "shadow-only" : "hybrid-shadowed" },
        { "hybridDX12Ready", DX12HybridReady() },
        { "hybridFallbackReason", composition.enabled &&
            WantsNativeHybrid() && !DX12HybridReady() ?
            "DX12 HybridGI unavailable; Shadow Only fallback selected" :
            (composition.enabled && !GetTierPolicy(composition.mode).ready ?
            "Selected transport tier not implemented: safe Shadow Only fallback" : "") },
        { "indirectRestriction", BlocksRTIndirect() ? "disabled-by-compositor-policy" : "" },
        { "rtExclusiveAllowed", !BlocksRTIndirect() },
        { "dx11HybridAllowed", false },
        { "ssgiAvailable", ssgiAvailable },
        { "giProviderStatus", effective != EffectiveMode::Legacy ?
            (ssgiAvailable && effective == EffectiveMode::ShadowOnly ? "baseline-ssgi" :
             effective == EffectiveMode::ShadowOnly ? "no-active-indirect-provider" : "dx12-hybrid") : "legacy" },
        { "shadowInfluence", composition.enabled && IsFixedPreset() &&
            composition.mode <= CompositorMode::HybridShadowed ?
            GetTierPolicy(composition.mode).shadowInfluence : composition.shadowInfluence },
        { "directContribution", composition.enabled && IsFixedPreset() &&
            composition.mode <= CompositorMode::HybridShadowed ?
            GetTierPolicy(composition.mode).directContribution : composition.rtDirectContribution },
        { "effectiveShadowTraceMode", GetTierPolicy(
            effective == EffectiveMode::HybridShadowed ?
                CompositorMode::HybridShadowed : CompositorMode::ShadowOnly).directTraceMode },
        { "effectiveDirectRREnabled", effective == EffectiveMode::Legacy &&
            globals::features::rtDirectLighting.settings.UseDirectRayRegeneration &&
            RayRegeneration::GetSettings().enabled },
        { "giContribution", composition.giContribution },
        { "nativeProductionRequested", composition.enabled && WantsNativeHybrid() },
        { "directCheckerboard", composition.directCheckerboard },
        { "hybridResolution", composition.hybridResolution },
        { "hybridCheckerboard", composition.hybridCheckerboard },
        { "effectiveHybridResolution", composition.enabled && IsFixedPreset() && WantsNativeHybrid() ?
            GetTierPolicy(CompositorMode::HybridShadowed).hybridResolution : composition.hybridResolution },
        { "effectiveHybridCheckerboard", composition.enabled && IsFixedPreset() && WantsNativeHybrid() ?
            GetTierPolicy(CompositorMode::HybridShadowed).hybridCheckerboard : composition.hybridCheckerboard },
        { "hybridSamplingMode", composition.hybridResolution },
        { "hybridSpatialScale", 1u << composition.hybridResolution },
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
            { "checkerboard", composition.directCheckerboard },
            { "resolutionMode", 0u },
            { "configuration", RayRegeneration::GetConfiguration().overrides }
        } },
        { "DirectRR", {
            { "UseDirectRayRegeneration", direct.UseDirectRayRegeneration },
            { "RRMaterialIsolation", direct.RRMaterialIsolation },
            { "RRGlossyRoughness", direct.RRGlossyRoughness }
        } },
        { "Composition", {
            { "enabled", composition.enabled },
            { "mode", static_cast<uint32_t>(composition.mode) },
            { "customHybridShadowed", composition.customHybridShadowed },
            { "shadowInfluence", composition.shadowInfluence },
            { "rtDirectContribution", composition.rtDirectContribution },
            { "giContribution", composition.giContribution },
            { "shadowThreshold", composition.shadowThreshold },
            { "directCheckerboard", composition.directCheckerboard },
            { "hybridResolution", composition.hybridResolution },
            { "hybridCheckerboard", composition.hybridCheckerboard },
            { "reconstruction", SaveHybridFilters(composition.reconstruction) }
        } }
    };
}

void RTComposer::LoadSettings(json& value)
{
    if (!value.is_object()) return;
    const auto beforePolicy = std::pair{ composition.enabled, composition.mode };
    const auto beforeSampling = std::tuple{ composition.directCheckerboard, composition.hybridResolution, composition.hybridCheckerboard, composition.customHybridShadowed };
    // Migrate older shared four-mode settings to two independent policies:
    // direct native checkerboard and SSGI-sized HybridGI with checkerboard.
    // Do not revive retired direct/RR/RT-exclusive resolution selectors.
    const auto legacyRR = value.value("FSRRayRegeneration", json::object());
    const auto currentRR = RayRegeneration::GetSettings();
    const uint32_t legacyReduction = legacyRR.is_object() ?
        legacyRR.value("resolutionMode", currentRR.resolutionMode) : currentRR.resolutionMode;
    const bool legacyCheckerboard = legacyRR.is_object() ?
        legacyRR.value("checkerboard", currentRR.checkerboard) : currentRR.checkerboard;
    const uint32_t migratedSampling = legacyReduction == 0u ?
        (legacyCheckerboard ? 1u : 0u) : (legacyCheckerboard ? 3u : 2u);
    // Each setting is optional: old profiles retain the disabled legacy path.
    if (auto it = value.find("Composition"); it != value.end() && it->is_object()) {
        CompositionSettings next{};
        next.enabled = it->value("enabled", false);
        const auto mode = it->value("mode", 0u);
        next.mode = static_cast<CompositorMode>(std::min(mode, 6u));
        next.customHybridShadowed = it->value("customHybridShadowed", false);
        auto bounded = [&](const char* name, float fallback) {
            const float x = it->value(name, fallback);
            return std::isfinite(x) ? std::clamp(x, 0.0f, 1.0f) : fallback;
        };
        next.shadowInfluence = bounded("shadowInfluence", 0.8f);
        next.rtDirectContribution = bounded("rtDirectContribution",
            GetTierPolicy(next.mode).directContribution);
        next.giContribution = bounded("giContribution", 1.0f);
        next.shadowThreshold = bounded("shadowThreshold", 0.25f);
        const uint32_t oldSampling = std::min(it->value("samplingMode", migratedSampling), 3u);
        next.directCheckerboard = it->value("directCheckerboard", (oldSampling & 1u) != 0u);
        next.hybridResolution = std::min(it->value("hybridResolution",
            oldSampling >= 2u ? 1u : 0u), 2u);
        next.hybridCheckerboard = it->value("hybridCheckerboard", (oldSampling & 1u) != 0u);
        if (auto filters = it->find("reconstruction"); filters != it->end())
            LoadHybridFilters(*filters, next.reconstruction);
        composition = next;
    } else {
        composition = {};
        composition.directCheckerboard = (migratedSampling & 1u) != 0u;
        composition.hybridResolution = migratedSampling >= 2u ? 1u : 0u;
        composition.hybridCheckerboard = composition.directCheckerboard;
    }
    if (beforePolicy != std::pair{ composition.enabled, composition.mode } ||
        beforeSampling != std::tuple{ composition.directCheckerboard, composition.hybridResolution, composition.hybridCheckerboard, composition.customHybridShadowed })
        OnCompositionPolicyChanged();
    const bool enabledRR = legacyRR.is_object() ?
        legacyRR.value("enabled", currentRR.enabled) : currentRR.enabled;
    ApplySamplingPolicy(composition.directCheckerboard);
    RayRegeneration::Configure(0u, composition.directCheckerboard, enabledRR);
    if (legacyRR.is_object()) {
        if (auto cfg = legacyRR.find("configuration"); cfg != legacyRR.end() && cfg->is_object())
            globals::features::redstoneGI.GetDenoiser().SetConfiguration(*cfg);
    }
    if (auto it = value.find("DirectRR"); it != value.end() && it->is_object()) {
        auto& direct = globals::features::rtDirectLighting.settings;
        const auto beforeResolution = direct.DirectRRResolution;
        const auto beforeIsolation = direct.RRMaterialIsolation;
        const auto beforeRoughness = direct.RRGlossyRoughness;
        direct.UseDirectRayRegeneration = it->value("UseDirectRayRegeneration", direct.UseDirectRayRegeneration);
        direct.DirectRRResolution = 0u; // persisted legacy resolution is not authoritative
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
    const auto beforeSampling = std::tuple{ composition.directCheckerboard, composition.hybridResolution, composition.hybridCheckerboard, composition.customHybridShadowed };
    composition = {};
    if (beforePolicy != std::pair{ composition.enabled, composition.mode } ||
        beforeSampling != std::tuple{ composition.directCheckerboard, composition.hybridResolution, composition.hybridCheckerboard, composition.customHybridShadowed })
        OnCompositionPolicyChanged();
    RayRegeneration::Configure(0u, false, true);
    globals::features::redstoneGI.GetDenoiser().SetConfiguration(json::object());
    auto& direct = globals::features::rtDirectLighting.settings;
    direct.UseDirectRayRegeneration = true;
    direct.DirectRRResolution = 0u;
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
    if (ImGui::Combo("Compositor mode", &compositorMode,
        "Shadow Only\0Hybrid-Shadowed\0RT-exclusive Tier I (pending)\0RT-exclusive Tier II (pending)\0RT-Enhanced (pending)\0FSR-REDSTONE (pending)\0Custom\0")) {
        // Never copy fixed-tier values into the saved Custom settings.
        composition.mode = static_cast<CompositorMode>(compositorMode);
    }
    if (previousPolicy != std::pair{ composition.enabled, composition.mode })
        OnCompositionPolicyChanged();

    if (IsFixedPreset()) {
        ImGui::TextWrapped("Fixed tier: compositor tuning is locked. Select Custom to edit direct, shadow, indirect, reconstruction or RR settings. Legacy feature settings remain editable while the compositor is disabled.");
        ImGui::TextWrapped("Direct: %s", DirectTierDescription(composition.mode));
        ImGui::TextWrapped("Indirect: %s", IndirectTierDescription(composition.mode));
        if (!GetTierPolicy(composition.mode).ready)
            ImGui::TextWrapped("NOT IMPLEMENTED: the effective renderer falls back to Shadow Only; no requested bounce transport is dispatched.");
        if (composition.mode == CompositorMode::FSRRedstone)
            ImGui::TextWrapped("High-end latest-generation AMD GPU recommended. FSR Radiance Cache integration is deferred.");
        if (composition.mode == CompositorMode::ShadowOnly || composition.mode == CompositorMode::HybridShadowed) {
            const auto preset = GetTierPolicy(composition.mode);
            ImGui::Text("Shadow contribution: %.2f | direct/raster blend: %.2f (fixed)",
                preset.shadowInfluence, preset.directContribution);
        }
    } else {
        ImGui::TextWrapped("Custom profile: these controls are persistent and are not overwritten by switching to a fixed tier.");
        if (ImGui::Checkbox("Custom: use native DX12 Hybrid-Shadowed GI", &composition.customHybridShadowed))
            OnCompositionPolicyChanged();
        if (ImGui::SliderFloat("Custom shadow influence", &composition.shadowInfluence, 0.0f, 1.0f, "%.2f"))
            OnCompositionPolicyChanged();
        ImGui::SliderFloat("Custom direct/raster blend", &composition.rtDirectContribution, 0.0f, 1.0f, "%.2f");
        if (ImGui::Checkbox("Custom direct native checkerboard (50%)", &composition.directCheckerboard)) {
            ApplySamplingPolicy(composition.directCheckerboard);
            OnCompositionPolicyChanged();
        }
        int giResolution = int(composition.hybridResolution);
        bool giChanged = ImGui::Combo("Custom HybridGI resolution (SSGI sizes)", &giResolution,
            "Full (100% W/H)\0Half (50% W/H)\0Quarter (25% W/H)\0");
        if (giChanged) composition.hybridResolution = uint32_t(giResolution);
        giChanged |= ImGui::Checkbox("Custom HybridGI checkerboard", &composition.hybridCheckerboard);
        if (giChanged) OnCompositionPolicyChanged();
        ImGui::TextWrapped("HybridGI RR operates on its selected logical grid and reconstructs native-resolution output on the DX12 compositor.");
    }

    if (WantsNativeHybrid()) {
        if (composition.mode == CompositorMode::Custom) {
            ImGui::SliderFloat("Custom native GI contribution", &composition.giContribution, 0.0f, 1.0f, "%.2f");
            ImGui::SeparatorText("Custom post-RR spatial reconstruction");
            constexpr std::array familyNames{ "Indirect diffuse", "Indirect specular", "Ambient/specular AO" };
            for (size_t family = 0; family < composition.reconstruction.size(); ++family) {
                ImGui::PushID(static_cast<int>(family));
                auto& settings = composition.reconstruction[family];
                if (ImGui::TreeNode(familyNames[family])) {
                    ImGui::Checkbox("Edge-aware filter", &settings.filterEnabled);
                    ImGui::SliderFloat("Filter radius", &settings.filterRadius, 0.5f, 4.0f, "%.1f px");
                    ImGui::SliderFloat("Filter strength", &settings.filterStrength, 0.0f, 1.0f);
                    ImGui::SliderFloat("Depth sensitivity", &settings.depthSensitivity, 1.0f, 256.0f);
                    ImGui::SliderFloat("Normal sensitivity", &settings.normalSensitivity, 1.0f, 128.0f);
                    ImGui::SliderFloat("Roughness sensitivity", &settings.roughnessSensitivity, 0.0f, 64.0f);
                    ImGui::Checkbox("Post-filter blur", &settings.blurEnabled);
                    ImGui::SliderFloat("Blur radius", &settings.blurRadius, 0.5f, 3.0f, "%.1f px");
                    ImGui::SliderFloat("Blur strength", &settings.blurStrength, 0.0f, 1.0f);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (ImGui::SliderFloat("Custom GI skip visibility threshold", &composition.shadowThreshold, 0.0f, 1.0f, "%.2f"))
                OnCompositionPolicyChanged();
            globals::features::redstoneGI.DrawSettings();
        }
        ImGui::TextWrapped("Native DX12 HybridGI diagnostics (production when scene, shaders and RR are ready).");
        if (!DX12HybridReady())
            ImGui::TextWrapped("Native DX12 scene or shaders unavailable; effective mode is Shadow Only.");
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
        ImGui::Text("GPU temporal surface-validation dispatches: %llu",
            static_cast<unsigned long long>(trace.value("nativeHybridHistoryValidationDispatches", uint64_t{0})));
        const auto historyFailure = trace.value("nativeHybridHistoryFailure", std::string{});
        if (!historyFailure.empty() &&
            RayRegeneration::GetSettings().enabled && trace.value("nativeHybridRRDispatches", uint64_t{0}) == 0)
            ImGui::TextWrapped("Native RR history: %s", historyFailure.c_str());
        ImGui::Text("Native RR frames / resets: %llu / %llu",
            static_cast<unsigned long long>(trace.value("nativeHybridRRDispatches", uint64_t{0})),
            static_cast<unsigned long long>(trace.value("nativeHybridRRResets", uint64_t{0})));
        ImGui::Text("Native post-RR filter / blur dispatches: %llu / %llu",
            static_cast<unsigned long long>(trace.value("nativeHybridPostDispatches", uint64_t{0})),
            static_cast<unsigned long long>(trace.value("nativeHybridBlurDispatches", uint64_t{0})));
        const auto nativeRRFailure = trace.value("nativeHybridRRFailure", std::string{});
        const auto nativePostFailure = trace.value("nativeHybridPostFailure", std::string{});
        if (!nativeRRFailure.empty())
            ImGui::TextWrapped("Native FSR RR: %s", nativeRRFailure.c_str());
        if (!nativePostFailure.empty())
            ImGui::TextWrapped("Post-RR reconstruction: %s", nativePostFailure.c_str());
        const auto guideFailure = trace.value("nativeHybridRRGuideFailure", std::string{});
        if (!guideFailure.empty())
            ImGui::TextWrapped("Native RR guide preparation: %s", guideFailure.c_str());
        const auto logical = trace.value("nativeHybridLogicalGrid", std::array<uint32_t, 2>{});
        const auto packed = trace.value("nativeHybridPackedGrid", std::array<uint32_t, 2>{});
        ImGui::Text("Hybrid RR grid: %u x %u | trace storage: %u x %u",
            logical[0], logical[1], packed[0], packed[1]);
        ImGui::Text("Active traced pixels: %llu | sampling phase: %u | checkerboard origin: %u",
            static_cast<unsigned long long>(trace.value("nativeHybridTracedPixels", uint64_t{0})),
            trace.value("nativeHybridSamplePhase", 0u),
            trace.value("nativeHybridCheckerboardOrigin", 0u));
        ImGui::Text("Native completed-scene exports: %llu",
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
        ImGui::TextWrapped("Shadow Only uses the raster scene and baseline SSGI where enabled. Unknown shadow visibility stays neutral.");
    }
    if (composition.enabled && IsFixedPreset()) {
        ImGui::TextWrapped("This is a fixed profile. Direct and indirect settings sections are read-only; Custom unlocks their controls.");
    } else {
        ImGui::SeparatorText("FSR Ray Regeneration");
        const auto old = RayRegeneration::GetSettings();
        bool enabled = old.enabled;
        if (ImGui::Checkbox("Enable FSR Ray Regeneration", &enabled)) {
            RayRegeneration::Configure(0u, composition.directCheckerboard, enabled);
            if (!enabled)
                (void)globals::features::redstoneGI.GetDenoiser().RetireNativeHybridContexts();
        }
        ImGui::SeparatorText("Direct Lighting RR");
        globals::features::rtDirectLighting.DrawRayRegenerationSettings();
        ImGui::SeparatorText("RR Signals and Diagnostics");
        globals::features::rtIndirectLighting.DrawUnifiedDebugSettings();
        if (!globals::features::rtIndirectLighting.UsesRTExclusive()) {
            ImGui::SeparatorText("Hybrid GI / AO Ray Regeneration");
            globals::features::redstoneGI.DrawRayRegenerationSettings();
        }
    }
    ImGui::PopID();
}

void RTComposer::RegisterUxActions()
{
    auto& registry = Util::DevBenchUx::Registry::GetSingleton();
    registry.RegisterCommand(GetShortName(), "setComposition",
        "Select compositor preset 0..6 (6=Custom); only Custom permits manual tuning.",
        +[](Feature* self, const json& args) {
            auto& composer = *static_cast<RTComposer*>(self);
            auto& policy = composer.composition;
            uint32_t requested = static_cast<uint32_t>(policy.mode);
            if (args.contains("mode")) requested = args.at("mode").get<uint32_t>();
            if (requested > 6u) throw std::invalid_argument("Unknown RT compositor mode");
            if (args.contains("samplingMode"))
                throw std::invalid_argument("Old samplingMode retired. Use Custom directCheckerboard and HybridGI controls.");
            constexpr std::array tunable{ "shadowInfluence", "rtDirectContribution",
                "shadowThreshold", "giContribution", "directCheckerboard",
                "hybridResolution", "hybridCheckerboard", "customHybridShadowed" };
            if (requested != static_cast<uint32_t>(CompositorMode::Custom))
                for (const char* key : tunable)
                    if (args.contains(key))
                        throw std::invalid_argument("Fixed compositor tier settings are locked; select Custom (mode 6) to tune.");
            const auto previous = std::tuple{ policy.enabled, policy.mode,
                policy.customHybridShadowed, policy.directCheckerboard,
                policy.hybridResolution, policy.hybridCheckerboard };
            // Validate everything before mutating policy to avoid partial updates.
            auto bounded = [&](const char* key, float fallback) {
                if (!args.contains(key)) return fallback;
                const float value = args.at(key).get<float>();
                if (!std::isfinite(value)) throw std::invalid_argument("Composition value must be finite");
                return std::clamp(value, 0.0f, 1.0f);
            };
            const float shadow = bounded("shadowInfluence", policy.shadowInfluence);
            const float direct = bounded("rtDirectContribution", policy.rtDirectContribution);
            const float threshold = bounded("shadowThreshold", policy.shadowThreshold);
            const float gi = bounded("giContribution", policy.giContribution);
            const uint32_t resolution = args.value("hybridResolution", policy.hybridResolution);
            if (resolution > 2u) throw std::invalid_argument("HybridGI resolution must be 0 full, 1 half or 2 quarter");
            const bool checkerboard = args.value("directCheckerboard", policy.directCheckerboard);
            const bool hybridChecker = args.value("hybridCheckerboard", policy.hybridCheckerboard);
            const bool customHybrid = args.value("customHybridShadowed", policy.customHybridShadowed);
            const bool enabled = args.value("enabled", policy.enabled);
            policy.mode = static_cast<CompositorMode>(requested);
            policy.enabled = enabled;
            if (policy.mode == CompositorMode::Custom) {
                policy.shadowInfluence = shadow;
                policy.rtDirectContribution = direct;
                policy.shadowThreshold = threshold;
                policy.giContribution = gi;
                policy.directCheckerboard = checkerboard;
                policy.hybridResolution = resolution;
                policy.hybridCheckerboard = hybridChecker;
                policy.customHybridShadowed = customHybrid;
            }
            if (previous != std::tuple{ policy.enabled, policy.mode,
                policy.customHybridShadowed, policy.directCheckerboard,
                policy.hybridResolution, policy.hybridCheckerboard }) {
                if (policy.mode == CompositorMode::Custom)
                    ApplySamplingPolicy(policy.directCheckerboard);
                composer.OnCompositionPolicyChanged();
            }
        });
    registry.RegisterCommand(GetShortName(), "setRayRegeneration",
        "Configure directCheckerboard, hybridResolution (0 full, 1 half, 2 quarter), hybridCheckerboard, and RR enablement.",
        +[](Feature* self, const json& args) {
            auto& composer = *static_cast<RTComposer*>(self);
            if (composer.TierControlsLocked())
                throw std::invalid_argument("Fixed compositor RR controls are locked; select Custom (mode 6).");
            if (args.contains("resolutionMode") || args.contains("checkerboard") ||
                args.contains("samplingMode"))
                throw std::invalid_argument("Old RR resolution controls retired. Use directCheckerboard, hybridResolution and hybridCheckerboard.");
            const auto oldRR = RayRegeneration::GetSettings();
            auto& policy = composer.composition;
            const auto previous = std::tuple{ policy.directCheckerboard, policy.hybridResolution, policy.hybridCheckerboard };
            if (args.contains("directCheckerboard"))
                policy.directCheckerboard = args.at("directCheckerboard").get<bool>();
            if (args.contains("hybridResolution")) {
                const uint32_t mode = args.at("hybridResolution").get<uint32_t>();
                if (mode > 2u) throw std::invalid_argument("Hybrid GI resolution must be 0 full, 1 half or 2 quarter");
                policy.hybridResolution = mode;
            }
            if (args.contains("hybridCheckerboard"))
                policy.hybridCheckerboard = args.at("hybridCheckerboard").get<bool>();
            ApplySamplingPolicy(policy.directCheckerboard);
            RayRegeneration::Configure(0u, policy.directCheckerboard,
                args.value("enabled", oldRR.enabled));
            if (previous != std::tuple{ policy.directCheckerboard, policy.hybridResolution, policy.hybridCheckerboard })
                composer.OnCompositionPolicyChanged();
        });
}

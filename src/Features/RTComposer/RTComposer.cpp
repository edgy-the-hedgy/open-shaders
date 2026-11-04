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
        return "Quarter checkerboard (1/8 native pixels per frame), temporal shadows/contact AA and tiny emitter oscillation; 0.9 shadow influence, 0.05 RT/raster blend. No FSR-RR is dispatched in this mode; the separate temporal shadow denoiser stays active.";
    case CompositorMode::HybridShadowed:
        return "Quarter-checkerboard direct/shadow DXR (12.5% pixels each frame), lightweight temporal shadow denoising plus contact-edge AA; tiny oscillating shadow emitter; 0.9 shadow influence, RT/raster direct blend 0.3. Diffuse-only direct with no direct FSR-RR; native HybridGI retains its separate indirect/AO RR.";
    case CompositorMode::RTExclusiveI:
        return "Quarter-density DXR direct/shadow and diffuse-only indirect with 50% survival; native reconstruction when optional Indirect RR is OFF or fails.";
    case CompositorMode::RTExclusiveII:
        return "Quarter-density DXR direct/shadow and 50% indirect survival with diffuse/GGX selection. Direct RR is pending; indirect RR optional.";
    case CompositorMode::RTEnhanced:
        return "Native checkerboard RT, 50% indirect survival; first native composition feeds quarter-size checkerboard HybridGI with separate RR histories.";
    case CompositorMode::FSRRedstone:
        return "Unavailable in this phase. Saved configuration is retained; runtime uses an explicitly reported Shadow Only fallback. Radiance Cache deferred.";
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
        return "Native DX12 HybridGI: quarter width/height by default, checkerboard off. Indirect diffuse/specular, AO and specular occlusion use RR or native DX12 reconstruction.";
    case CompositorMode::RTExclusiveI:
        return "Diffuse-only stochastic transport packed for native RR and composed in DX12. Invalid frames retain the raster scene.";
    case CompositorMode::RTExclusiveII:
        return "50% survival; independently valid diffuse/specular families pass through native RR into DX12 final composition.";
    case CompositorMode::RTEnhanced:
        return "50% RT first-bounce survival, linear native scene composition and quarter-size checkerboard follow-up HybridGI.";
    case CompositorMode::FSRRedstone:
        return "66% first and 25% second-bounce continuation, native RR and post-composite HybridGI; FSR-RC deferred.";
    case CompositorMode::Custom:
        return "Custom: select Shadow Only or DX12 Hybrid-Shadowed in RT Composer; edit indirect and AO sampling, reconstruction and RR settings.";
    }
    return "Unknown compositor tier.";
}
std::pair<std::string, std::vector<std::string>> RTComposer::GetFeatureSummary()
{
    return { "Open RT native reconstruction and independently selected FSR Ray Regeneration modes. Unfinished RR modes retain a safe baseline.", {} };
}

bool RTComposer::DX12HybridReady() const
{
    const auto& direct = globals::features::rtDirectLighting;
    const auto& native = globals::features::dx12NativeScene;
    return !globals::game::isVR && direct.loaded && direct.settings.UsePreparedDirectLighting &&
        native.loaded && native.settings.UsePreparedScene && native.settings.UsePreparedMaterials &&
        bool(native.GetPublishedScene()) &&
        DX12Native::NativeHybridGI::ShadersAvailable() &&
        // Native reconstruction shaders are prerequisites; AMD RR availability
        // is explicitly NOT a condition of native DX12 lighting readiness.
        (!IsFixedPreset() || composition.mode == CompositorMode::ShadowOnly ||
            DX12Native::NativeHybridGI::AdvancedShadersAvailable());
}
void RTComposer::OnReconstructionPolicyChanged(
    CompositorMode mode, uint32_t family, bool enabled)
{
    // New RR providers initialize lazily; disabling one releases only
    // its private SDK contexts after the existing queue-fence retirement.
    // Geometry, TLAS and the raw-lighting producer are left untouched.
    if (enabled || !composition.enabled || composition.mode != mode) return;
    if (family == 1u && (mode == CompositorMode::RTExclusiveI ||
        mode == CompositorMode::RTExclusiveII ||
        mode == CompositorMode::RTEnhanced))
        (void)globals::features::dx12NativeScene.Tracer().RetireAdvancedRRContexts();
    if (family == 2u && (mode == CompositorMode::HybridShadowed ||
        mode == CompositorMode::RTEnhanced))
        (void)globals::features::redstoneGI.GetDenoiser().RetireNativeHybridContexts();
}

void RTComposer::OnCompositionPolicyChanged()
{
    // This method handles changes to transport or compositor mode, not RR
    // checkbox changes. Retire only providers not requested by the new mode.
    const auto mode = composition.mode;
    const bool prototype = LegacyHybridPrototypeActive();
    const bool fixedHybridRR = prototype &&
        (mode == CompositorMode::HybridShadowed ||
         mode == CompositorMode::RTEnhanced) &&
        composition.fixedRR.Get(mode, 2u);
    const bool customHybridRR = prototype &&
        mode == CompositorMode::Custom && composition.customHybridShadowed &&
        composition.useFSRRayRegeneration &&
        RayRegeneration::GetSettings().enabled;
    const bool indirectRR = IsFSRRenderer() && composition.enabled &&
        (mode == CompositorMode::RTExclusiveI ||
         mode == CompositorMode::RTExclusiveII ||
         mode == CompositorMode::RTEnhanced) &&
        composition.fixedRR.Get(mode, 1u);
    if (!fixedHybridRR && !customHybridRR)
        (void)globals::features::redstoneGI.GetDenoiser().RetireNativeHybridContexts();
    if (!indirectRR)
        (void)globals::features::dx12NativeScene.Tracer().RetireAdvancedRRContexts();
    // Transport-policy changes can invalidate history. The RR-only setter
    // never invokes this broader reset.
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
    const auto planned = GetTierExecutionContract(composition.mode);
    return {
        { "enabled", composition.enabled },
        { "rendererFamily", RendererFamilyName(composition.rendererFamily) },
        { "openRTRequestedMode", static_cast<uint32_t>(composition.openRTMode) },
        { "fsrRRRequestedMode", FSRModeName(composition.fsrRRMode) },
        { "fsrProductionReady", false },
        { "fsrLegacyHybridPrototype", LegacyHybridPrototypeActive() },
        { "requestedMode", ModeName(composition.mode) },
        { "requestedTierReady", composition.mode == CompositorMode::Custom || GetTierPolicy(composition.mode).ready },
        { "tierControlsLocked", TierControlsLocked() },
        { "customHybridShadowed", composition.customHybridShadowed },
        { "firstBounceSurvival", GetTierPolicy(composition.mode).firstBounceSurvival },
        { "specularBounceFraction", GetTierPolicy(composition.mode).specularBounceFraction },
        { "secondBounceSurvival", GetTierPolicy(composition.mode).secondBounceSurvival },
        { "requestedAdvancedTierExecution", {
            { "directTraceMode", planned.directTraceMode },
            { "indirectTraceMode", planned.indirectTraceMode },
            { "indirectCandidateRaysPerAnchor", planned.indirectCandidateRays },
            { "rrSignalFamilyMask", planned.rrSignalFamilies },
            { "postCompositeHybridResolution", planned.hybridResolution },
            { "postCompositeHybridCheckerboard", planned.hybridCheckerboard },
            { "needsNativeComposition", planned.requiresNativeComposition },
            { "needsSparseFamilyValidity", planned.requiresPerFamilyValidity },
            { "needsPostCompositeHybridGI", planned.needsPostCompositeHybrid },
            { "needsSecondBounce", planned.needsSecondBounce }
        } },
        { "stagedTransportShaderTierID", composition.enabled && GetTierPolicy(composition.mode).ready ?
            0u : (composition.enabled && composition.mode >= CompositorMode::RTExclusiveI &&
            composition.mode <= CompositorMode::FSRRedstone ?
            static_cast<uint32_t>(composition.mode) : 0u) },
        { "stagedTransportDispatchEnabled", AdvancedTransportDiagnosticActive() || AdvancedActive() },
        { "advancedTransportDiagnosticRequested", composition.diagnoseAdvancedTransport },
        { "advancedTransportDispatches", tracer.GetDiagnostics().value(
            "advancedTransportDispatches", uint64_t{0}) },
        { "advancedPresentedFrames", tracer.GetDiagnostics().value(
            "advancedPresentedFrames", uint64_t{0}) },
        { "advancedRasterFallbackFrames", tracer.GetDiagnostics().value(
            "advancedRasterFallbackFrames", uint64_t{0}) },
        { "advancedTransportPublication",
            globals::game::graphicsState && tracer.GetTransportSignals(
                globals::game::graphicsState->GetFrameCount()) ? "gpu-fence-gated" : "not-published" },
        { "advancedTierContractReady", !IsAdvancedTransportTier(composition.mode) ||
            (GetTierPolicy(composition.mode).ready && DX12HybridReady()) },
        { "advancedTierBlockingInterfaces",
            IsAdvancedTransportTier(composition.mode) && !DX12HybridReady() ?
            "native scene or packaged DX12 transport/GI/reconstruction shaders unavailable" : "" },
        { "transportIntegrationStatus",
            "Native DX12 RT transport, FSR-RR and post-composite HybridGI scheduled; only completed GPU scenes are presented" },
        { "effectiveMode", effective == EffectiveMode::Legacy ? "legacy" :
            effective == EffectiveMode::ShadowOnly ? "shadow-only" :
            effective == EffectiveMode::HybridShadowed ? "hybrid-shadowed" :
            effective == EffectiveMode::RTExclusive ? "rt-exclusive" : "rt-enhanced" },
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
             effective == EffectiveMode::ShadowOnly ? "no-active-indirect-provider" :
             effective == EffectiveMode::RTExclusive ? "dx12-rt-exclusive" :
             effective == EffectiveMode::RTEnhanced ? "dx12-post-composite-hybrid" :
             "dx12-hybrid") : "legacy" },
        { "shadowInfluence", composition.enabled && IsFixedPreset() ?
            GetTierPolicy(composition.mode).shadowInfluence : composition.shadowInfluence },
        { "directContribution", composition.enabled && IsFixedPreset() ?
            GetTierPolicy(composition.mode).directContribution : composition.rtDirectContribution },
        { "effectiveShadowTraceMode", GetTierPolicy(
            effective == EffectiveMode::RTExclusive || effective == EffectiveMode::RTEnhanced ?
                composition.mode :
            effective == EffectiveMode::HybridShadowed ?
                CompositorMode::HybridShadowed : CompositorMode::ShadowOnly).directTraceMode },
        { "shadowContactFilterRadiusPixels", composition.enabled && IsFixedPreset() ? 1u : 0u },
        { "shadowContactFilterRequiresValidReceiver", true },
        { "shadowContactFilterDepthAware", composition.enabled && IsFixedPreset() },
        { "shadowTemporalFilterEnabled", composition.enabled &&
            (composition.mode == CompositorMode::ShadowOnly ||
             composition.mode == CompositorMode::HybridShadowed) },
        { "shadowTemporalHistoryWeight", 0.70f },
        { "compositorFSRRayRegenerationEnabled", composition.useFSRRayRegeneration },
        { "fixedRRRequested", {
            { "direct", composition.fixedRR.Get(composition.mode, 0u) },
            { "indirect", composition.fixedRR.Get(composition.mode, 1u) },
            { "hybrid", composition.fixedRR.Get(composition.mode, 2u) }
        } },
        { "advancedDeveloperMode", advancedDeveloperMode },
        { "developerReconstructionRevision", developerRevision },
        { "hybridGISceneSourceRequested",
            UseRasterHybridGISource() ? "raster-baseline" : "native-shadowed" },
        { "hybridGISceneSourceLastRecorded", tracer.GetDiagnostics().value(
            "hybridGIInputScene", std::string{"not-recorded"}) },
        { "hybridGISceneSourceLastRecordedFrame", tracer.GetDiagnostics().value(
            "hybridGIInputSceneFrame", uint32_t{0}) },
        { "developerPostHybridGIEnabled", !advancedDeveloperMode || developer.showPostHybridGI },
        { "developerPostHybridGIContribution",
            advancedDeveloperMode ? developer.scenePostGIContribution : 1.f },
        { "developerEffectiveSettings", {
            { "hybridGISceneSource",
                UseRasterHybridGISource() ? "raster-baseline" : "native-shadowed" },
            { "spatialDepthTolerance", EffectiveDeveloperControls().spatialDepthTolerance },
            { "spatialNormalThreshold", EffectiveDeveloperControls().spatialNormalThreshold },
            { "spatialRoughnessTolerance", EffectiveDeveloperControls().spatialRoughnessTolerance },
            { "spatialDistanceFalloff", EffectiveDeveloperControls().spatialDistanceFalloff },
            { "spatialDepthFalloff", EffectiveDeveloperControls().spatialDepthFalloff },
            { "spatialRoughnessFalloff", EffectiveDeveloperControls().spatialRoughnessFalloff },
            { "spatialNormalPower", EffectiveDeveloperControls().spatialNormalPower },
            { "historyDepthTolerance", EffectiveDeveloperControls().historyDepthTolerance },
            { "historyNormalThreshold", EffectiveDeveloperControls().historyNormalThreshold },
            { "historyRoughnessTolerance", EffectiveDeveloperControls().historyRoughnessTolerance },
            { "sceneBlendOverridden", DeveloperSceneOverride() },
            { "directBlend", DeveloperSceneOverride() ? developer.sceneDirectBlend :
                (IsFixedPreset() ? GetTierPolicy(composition.mode).directContribution :
                    composition.rtDirectContribution) },
            { "giContribution", DeveloperSceneOverride() ?
                developer.sceneGIContribution : (IsFixedPreset() ? 1.f : composition.giContribution) },
            { "shadowInfluence", DeveloperSceneOverride() ?
                developer.sceneShadowInfluence : (IsFixedPreset() ?
                    GetTierPolicy(composition.mode).shadowInfluence : composition.shadowInfluence) },
            { "postHybridGIEnabled", !advancedDeveloperMode || developer.showPostHybridGI },
            { "postHybridGIContribution", advancedDeveloperMode ?
                developer.scenePostGIContribution : 1.f },
            { "shadowTemporalEnabled", !advancedDeveloperMode ||
                !developer.overrideShadowTemporal || developer.shadowTemporalEnabled },
            { "shadowTemporalWeight", advancedDeveloperMode && developer.overrideShadowTemporal ?
                developer.shadowTemporalWeight : .70f },
            { "shadowTemporalDepthTolerance", advancedDeveloperMode && developer.overrideShadowTemporal ?
                developer.shadowTemporalDepthTolerance : .018f },
            { "contactAAEnabled", !advancedDeveloperMode || developer.shadowContactAAEnabled },
            { "contactAAWeight", EffectiveDeveloperControls().shadowContactAAWeight },
            { "contactAADepthTolerance", EffectiveDeveloperControls().shadowContactAADepthTolerance },
            { "postFilters", SaveHybridFilters(EffectiveReconstructionPolicy().reconstruction) }
        } },
        { "developerOverridesSessionOnly", true },
        { "developerSpatialEnabled", {
            EffectiveDeveloperControls().signals[0].spatialEnabled,
            EffectiveDeveloperControls().signals[1].spatialEnabled,
            EffectiveDeveloperControls().signals[2].spatialEnabled } },
        { "developerSpatialRadius", {
            EffectiveDeveloperControls().signals[0].spatialRadius,
            EffectiveDeveloperControls().signals[1].spatialRadius,
            EffectiveDeveloperControls().signals[2].spatialRadius } },
        { "developerTemporalEnabled", {
            EffectiveDeveloperControls().signals[0].temporalEnabled,
            EffectiveDeveloperControls().signals[1].temporalEnabled,
            EffectiveDeveloperControls().signals[2].temporalEnabled } },
        { "developerHistoryWeight", {
            EffectiveDeveloperControls().signals[0].historyWeight,
            EffectiveDeveloperControls().signals[1].historyWeight,
            EffectiveDeveloperControls().signals[2].historyWeight } },
        { "developerSceneMask", advancedDeveloperMode ? (
            (developer.showDirect ? 1u : 0u) |
            (developer.showDiffuse ? 2u : 0u) |
            (developer.showSpecular ? 4u : 0u) |
            (developer.useAO ? 8u : 0u)) : 15u },
        { "fixedRRProviderWired", false }, // native Direct RR remains deferred
        { "fixedRRProviderStatus", {
            { "direct", "pending-native-dx12-provider" },
            { "indirect", IsAdvancedTransportTier(composition.mode) &&
                GetTierPolicy(composition.mode).ready ? "native-fallback-wired-unverified" : "unavailable" },
            { "hybrid", (composition.mode == CompositorMode::HybridShadowed ||
                composition.mode == CompositorMode::RTEnhanced) ?
                "native-fallback-wired-unverified" : "unavailable" }
        } },
        { "currentHybridReconstructionProvider", tracer.GetDiagnostics().value(
            "nativeHybridReconstructionProvider", std::string{}) },
        { "currentPostHybridReconstructionProvider", tracer.GetDiagnostics().value(
            "postHybridReconstructionProvider", std::string{}) },
        { "effectiveDirectRREnabled",
            !TierControlsLocked() &&
            globals::features::rtDirectLighting.settings.UseDirectRayRegeneration &&
            RayRegeneration::GetSettings().enabled },
        { "lowTierStochasticRadiusMultiplier", composition.enabled &&
            (composition.mode == CompositorMode::ShadowOnly ||
             composition.mode == CompositorMode::HybridShadowed) ? 0.5f : 1.0f },
        { "lowTierShadowEmitterOscillation", composition.enabled &&
            (composition.mode == CompositorMode::ShadowOnly ||
             composition.mode == CompositorMode::HybridShadowed) },
        { "lowTierShadowEmitterOscillationRadiusFraction", 0.0075f },
        { "giContribution", composition.giContribution },
        { "nativeProductionRequested", composition.enabled && WantsNativeHybrid() },
        { "directCheckerboard", composition.directCheckerboard },
        { "hybridResolution", composition.hybridResolution },
        { "hybridCheckerboard", composition.hybridCheckerboard },
        { "effectiveHybridResolution", composition.enabled && IsFixedPreset() && WantsNativeHybrid() ?
            GetTierPolicy(AdvancedActive() ? composition.mode : CompositorMode::HybridShadowed).hybridResolution : composition.hybridResolution },
        { "effectiveHybridCheckerboard", composition.enabled && IsFixedPreset() && WantsNativeHybrid() ?
            GetTierPolicy(AdvancedActive() ? composition.mode : CompositorMode::HybridShadowed).hybridCheckerboard : composition.hybridCheckerboard },
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
            { "useFSRRayRegeneration", composition.useFSRRayRegeneration },
            { "fixedRRRequests", composition.fixedRR.modes },
            { "diagnoseAdvancedTransport", composition.diagnoseAdvancedTransport },
            { "mode", static_cast<uint32_t>(composition.mode) },
            { "rendererFamily", static_cast<uint32_t>(composition.rendererFamily) },
            { "openRTMode", static_cast<uint32_t>(composition.openRTMode) },
            { "fsrRRMode", static_cast<uint32_t>(composition.fsrRRMode) },
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
    const auto beforeFamily = composition.rendererFamily;
    const bool beforeTransportDiagnostic = composition.diagnoseAdvancedTransport;
    const bool beforeCompositorRR = composition.useFSRRayRegeneration;
    const auto oldFixedRR = composition.fixedRR.modes;
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
        next.useFSRRayRegeneration = it->value("useFSRRayRegeneration", true);
        // Do not migrate the legacy universal RR switch into fixed-mode requests.
        // New requests default OFF, including when a prior universal value was true.
        if (auto saved = it->find("fixedRRRequests"); saved != it->end() && saved->is_array()) {
            for (size_t modeIndex = 0; modeIndex < std::min(saved->size(), next.fixedRR.modes.size()); ++modeIndex) {
                if (!(*saved)[modeIndex].is_array()) continue;
                for (uint32_t family = 0; family < 3u && family < (*saved)[modeIndex].size(); ++family) {
                    if ((*saved)[modeIndex][family].is_boolean())
                        (void)next.fixedRR.Set(static_cast<CompositorMode>(modeIndex),
                            family, (*saved)[modeIndex][family].get<bool>());
                }
            }
        }
        next.diagnoseAdvancedTransport = it->value("diagnoseAdvancedTransport", false);
        const auto mode = it->value("mode", 0u);
        next.mode = static_cast<CompositorMode>(std::min(mode, 6u));
        if (it->contains("rendererFamily")) {
            next.rendererFamily = it->value("rendererFamily", 0u) == 1u ?
                RendererFamily::FSRRayRegeneration : RendererFamily::OpenRT;
            next.openRTMode = static_cast<OpenRTMode>(
                std::min(it->value("openRTMode", 0u), 3u));
            next.fsrRRMode = static_cast<FSRRRMode>(
                std::min(it->value("fsrRRMode", 0u), 2u));
        } else {
            // Legacy selection migration is non-destructive: keep saved
            // settings but never interpret old RR tiers as completed new RR.
            switch (next.mode) {
            case CompositorMode::RTExclusiveI:
                next.openRTMode = OpenRTMode::RTExclusiveI; break;
            case CompositorMode::RTExclusiveII:
                next.openRTMode = OpenRTMode::RTExclusiveII; break;
            case CompositorMode::Custom:
                next.openRTMode = OpenRTMode::Custom; break;
            case CompositorMode::HybridShadowed:
                next.rendererFamily = RendererFamily::FSRRayRegeneration;
                next.fsrRRMode = FSRRRMode::HybridIllumination; break;
            case CompositorMode::RTEnhanced:
                next.rendererFamily = RendererFamily::FSRRayRegeneration;
                next.fsrRRMode = FSRRRMode::RTEnhanced; break;
            case CompositorMode::FSRRedstone:
                next.rendererFamily = RendererFamily::FSRRayRegeneration;
                next.fsrRRMode = FSRRRMode::FSRRedstone; break;
            default: break;
            }
        }
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
    SynchronizeRendererFamily();
    if (beforeFamily != composition.rendererFamily ||
        beforePolicy != std::pair{ composition.enabled, composition.mode } ||
        beforeTransportDiagnostic != composition.diagnoseAdvancedTransport ||
        (beforeCompositorRR != composition.useFSRRayRegeneration &&
         composition.mode == CompositorMode::Custom) ||
        beforeSampling != std::tuple{ composition.directCheckerboard, composition.hybridResolution, composition.hybridCheckerboard, composition.customHybridShadowed })
        OnCompositionPolicyChanged();
    else if (oldFixedRR != composition.fixedRR.modes) {
        const uint32_t mode = static_cast<uint32_t>(composition.mode);
        if (mode < composition.fixedRR.modes.size())
            for (uint32_t family = 0u; family < 3u; ++family)
                if (oldFixedRR[mode][family] != composition.fixedRR.modes[mode][family])
                    OnReconstructionPolicyChanged(composition.mode, family,
                        composition.fixedRR.modes[mode][family]);
    }
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
    const bool beforeTransportDiagnostic = composition.diagnoseAdvancedTransport;
    const bool beforeCompositorRR = composition.useFSRRayRegeneration;
    const auto oldFixedRR = composition.fixedRR.modes;
    const auto beforeSampling = std::tuple{ composition.directCheckerboard, composition.hybridResolution, composition.hybridCheckerboard, composition.customHybridShadowed };
    composition = {};
    // A normal Reset Defaults must always restore strict-mode behavior.
    if (advancedDeveloperMode || !(developer == DeveloperControls{})) {
        advancedDeveloperMode = false;
        developer = DeveloperControls{};
        ++developerRevision;
    }
    if (beforePolicy != std::pair{ composition.enabled, composition.mode } ||
        beforeTransportDiagnostic != composition.diagnoseAdvancedTransport ||
        (beforeCompositorRR != composition.useFSRRayRegeneration &&
         composition.mode == CompositorMode::Custom) ||
        beforeSampling != std::tuple{ composition.directCheckerboard, composition.hybridResolution, composition.hybridCheckerboard, composition.customHybridShadowed })
        OnCompositionPolicyChanged();
    else if (oldFixedRR != composition.fixedRR.modes) {
        const uint32_t mode = static_cast<uint32_t>(composition.mode);
        if (mode < composition.fixedRR.modes.size())
            for (uint32_t family = 0u; family < 3u; ++family)
                if (oldFixedRR[mode][family])
                    OnReconstructionPolicyChanged(composition.mode, family, false);
    }
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
    ImGui::SeparatorText("Renderer architecture");
    const auto previousPolicy = std::tuple{ composition.enabled, composition.mode,
        composition.rendererFamily };
    ImGui::Checkbox("Enable RT composition", &composition.enabled);
    int family = static_cast<int>(composition.rendererFamily);
    if (ImGui::Combo("Renderer family", &family,
        "Open RT (Built-in Denoising)\0FSR Ray Regeneration\0"))
        composition.rendererFamily = static_cast<RendererFamily>(family);
    if (IsOpenRT()) {
        int selection = static_cast<int>(composition.openRTMode);
        if (ImGui::Combo("Open RT mode", &selection,
            "Shadow Only\0RT-exclusive Tier I\0RT-exclusive Tier II\0Custom\0"))
            composition.openRTMode = static_cast<OpenRTMode>(selection);
        ImGui::TextWrapped("Open RT uses native DX12 reconstruction; no FSR-RR contexts are dispatched by this renderer.");
    } else {
        int selection = static_cast<int>(composition.fsrRRMode);
        if (ImGui::Combo("FSR Ray Regeneration mode", &selection,
            "Hybrid-Illumination\0RT-Enhanced\0FSR-REDSTONE (unavailable)\0"))
            composition.fsrRRMode = static_cast<FSRRRMode>(selection);
        ImGui::TextWrapped("FSR-RR raw Direct and two-bounce transport is under construction. The selected request is saved independently. Production uses safe Shadow Only until these native DX12 RR providers are ready.");
        if (composition.fsrRRMode == FSRRRMode::FSRRedstone)
            ImGui::TextWrapped("FSR-REDSTONE is unavailable.");
    }
    SynchronizeRendererFamily();
    if (previousPolicy != std::tuple{ composition.enabled, composition.mode,
        composition.rendererFamily })
        OnCompositionPolicyChanged();

    ImGui::SeparatorText("Developer testing");
    const auto priorDeveloperSettings = developer;
    const bool priorDeveloperMode = advancedDeveloperMode;
    ImGui::Checkbox("Advanced developer mode (session only)", &advancedDeveloperMode);
    ImGui::TextWrapped(advancedDeveloperMode ?
        "Developer overrides are ACTIVE. All changes are temporary and do not modify fixed preset values, tracing eligibility, geometry, or saved settings." :
        "Strict compositor policies are ACTIVE. Developer settings are ignored until enabled.");
    if (advancedDeveloperMode) {
        if (IsFSRRenderer() &&
            composition.fsrRRMode == FSRRRMode::HybridIllumination) {
            ImGui::SeparatorText("HybridGI baseline research");
            ImGui::Checkbox("Enable legacy HybridGI prototype (not RR-only)",
                &developer.legacyHybridGIPrototype);
            ImGui::TextWrapped("Research only: this uses the old DX12 Hybrid-Shadowed path and native RR fallback, not the new raw Direct Diffuse/Specular RR pipeline.");
        }
        if (ImGui::Button("Reset developer test parameters"))
            developer = DeveloperControls{};
        if (WantsNativeHybrid() &&
            (composition.mode == CompositorMode::HybridShadowed ||
             composition.mode == CompositorMode::Custom)) {
            ImGui::SeparatorText("HybridGI input scene");
            ImGui::Checkbox("Use Raster Baseline instead of Native Shadowed",
                &developer.hybridGIRasterBaseline);
            ImGui::TextWrapped("Raster Baseline reads the captured raster scene; Native Shadowed reads DX12 shadow-adjusted linear radiance. Final composition remains shadowed in both cases. This is a source-quality A/B switch; the existing source/depth preparation remains scheduled.");
            const auto trace = globals::features::dx12NativeScene.Tracer().GetDiagnostics();
            const auto recorded = trace.value("hybridGIInputScene", std::string{"not-recorded"});
            ImGui::Text("Last GI source recorded: %s", recorded.c_str());
        }
        if (ImGui::TreeNode("Native spatial reconstruction")) {
            constexpr std::array names{ "Diffuse", "Specular", "AO / Specular AO" };
            for (int family = 0; family < 3; ++family) {
                ImGui::PushID(family);
                ImGui::TextUnformatted(names[family]);
                ImGui::Checkbox("Spatial reconstruction", &developer.signals[family].spatialEnabled);
                ImGui::SliderInt("Spatial radius (logical pixels)", &developer.signals[family].spatialRadius, 0, 4);
                ImGui::PopID();
            }
            ImGui::SliderFloat("Spatial relative depth tolerance", &developer.spatialDepthTolerance, .001f, .1f, "%.4f");
            ImGui::SliderFloat("Spatial minimum normal dot", &developer.spatialNormalThreshold, 0.f, 1.f, "%.3f");
            ImGui::SliderFloat("Spatial roughness difference max", &developer.spatialRoughnessTolerance, 0.f, 1.f, "%.3f");
            ImGui::SliderFloat("Spatial distance falloff", &developer.spatialDistanceFalloff, 0.f, 2.f);
            ImGui::SliderFloat("Spatial depth falloff", &developer.spatialDepthFalloff, 0.f, 128.f);
            ImGui::SliderFloat("Spatial roughness falloff", &developer.spatialRoughnessFalloff, 0.f, 64.f);
            ImGui::SliderFloat("Spatial normal exponent", &developer.spatialNormalPower, 0.f, 64.f);
            ImGui::TextWrapped("Disabling spatial recovery preserves actual traced hits and misses, but leaves untraced pixels invalid rather than inventing lighting.");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Native temporal reconstruction")) {
            constexpr std::array names{ "Diffuse", "Specular", "AO / Specular AO" };
            for (int family = 0; family < 3; ++family) {
                ImGui::PushID(10 + family);
                ImGui::TextUnformatted(names[family]);
                ImGui::Checkbox("Temporal history", &developer.signals[family].temporalEnabled);
                ImGui::SliderFloat("Previous frame contribution", &developer.signals[family].historyWeight, 0.f, 1.f, "%.2f");
                ImGui::PopID();
            }
            ImGui::SeparatorText("Temporal same-surface rejection");
            ImGui::SliderFloat("History relative depth tolerance", &developer.historyDepthTolerance, .001f, .1f, "%.4f");
            ImGui::SliderFloat("History minimum normal dot", &developer.historyNormalThreshold, 0.f, 1.f, "%.3f");
            ImGui::SliderFloat("History roughness difference max", &developer.historyRoughnessTolerance, 0.f, 1.f, "%.3f");
            ImGui::TextWrapped("Previous-frame geometry/motion checks remain mandatory, even in developer mode.");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Post-reconstruction filter / blur")) {
            if (ImGui::Checkbox("Override post-filter presets", &developer.overridePost))
                developer.post = IsFixedPreset() ?
                    std::array<SignalReconstructionSettings, 3>{} :
                    composition.reconstruction;
            if (developer.overridePost) {
                constexpr std::array names{ "Diffuse", "Specular", "AO / Specular AO" };
                for (int family = 0; family < 3; ++family) {
                    ImGui::PushID(30 + family);
                    if (ImGui::TreeNode(names[family])) {
                        auto& p = developer.post[family];
                        ImGui::Checkbox("Edge-aware filter", &p.filterEnabled);
                        ImGui::SliderFloat("Filter radius (native pixels)", &p.filterRadius, .5f, 4.f);
                        ImGui::SliderFloat("Filter strength", &p.filterStrength, 0.f, 1.f);
                        ImGui::SliderFloat("Depth sensitivity", &p.depthSensitivity, 1.f, 256.f);
                        ImGui::SliderFloat("Normal sensitivity", &p.normalSensitivity, 1.f, 128.f);
                        ImGui::SliderFloat("Roughness sensitivity", &p.roughnessSensitivity, 0.f, 64.f);
                        ImGui::Checkbox("Post-filter blur", &p.blurEnabled);
                        ImGui::SliderFloat("Blur radius", &p.blurRadius, .5f, 3.f);
                        ImGui::SliderFloat("Blur strength", &p.blurStrength, 0.f, 1.f);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("DX12 scene composition")) {
            ImGui::Checkbox("Include native direct", &developer.showDirect);
            ImGui::Checkbox("Include indirect diffuse", &developer.showDiffuse);
            ImGui::Checkbox("Include indirect specular", &developer.showSpecular);
            ImGui::Checkbox("Apply sampled AO", &developer.useAO);
            ImGui::Checkbox("Include RT-Enhanced post-composite HybridGI", &developer.showPostHybridGI);
            ImGui::SliderFloat("RT-Enhanced post-composite GI contribution",
                &developer.scenePostGIContribution, 0.f, 1.f, "%.2f");
            if (ImGui::Checkbox("Override scene blend values", &developer.overrideSceneBlend) &&
                developer.overrideSceneBlend) {
                const auto tier = GetTierPolicy(composition.mode);
                developer.sceneDirectBlend = IsFixedPreset() ? tier.directContribution : composition.rtDirectContribution;
                developer.sceneGIContribution = IsFixedPreset() ? 1.f : composition.giContribution;
                developer.sceneShadowInfluence = IsFixedPreset() ? tier.shadowInfluence : composition.shadowInfluence;
            }
            if (developer.overrideSceneBlend) {
                ImGui::SliderFloat("Direct / raster blend", &developer.sceneDirectBlend, 0.f, 1.f, "%.2f");
                ImGui::SliderFloat("Indirect GI contribution", &developer.sceneGIContribution, 0.f, 1.f, "%.2f");
                ImGui::SliderFloat("Shadow influence", &developer.sceneShadowInfluence, 0.f, 1.f, "%.2f");
            }
            ImGui::TextWrapped("These toggles affect only final DX12 lighting composition, not the prepared scene, rays, transport or HybridGI source selection.");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Low-tier temporal/contact shadows")) {
            ImGui::Checkbox("Override shadow temporal settings", &developer.overrideShadowTemporal);
            if (developer.overrideShadowTemporal) {
                ImGui::Checkbox("Temporal shadow reconstruction", &developer.shadowTemporalEnabled);
                ImGui::SliderFloat("Shadow history contribution", &developer.shadowTemporalWeight, 0.f, 1.f, "%.2f");
                ImGui::SliderFloat("Shadow history relative depth tolerance", &developer.shadowTemporalDepthTolerance, .001f, .1f, "%.4f");
            }
            ImGui::Checkbox("Contact-shadow edge AA", &developer.shadowContactAAEnabled);
            ImGui::SliderFloat("Contact-shadow AA blend", &developer.shadowContactAAWeight, 0.f, 1.f);
            ImGui::SliderFloat("Contact-shadow AA depth tolerance", &developer.shadowContactAADepthTolerance, .001f, .1f, "%.4f");
            ImGui::TreePop();
        }
    }
    if (advancedDeveloperMode != priorDeveloperMode ||
        !(developer == priorDeveloperSettings)) {
        ++developerRevision;
        if (SynchronizeRendererFamily())
            OnCompositionPolicyChanged();
    }
    if (advancedDeveloperMode && WantsNativeHybrid()) {
        const auto trace = globals::features::dx12NativeScene.Tracer().GetDiagnostics();
        ImGui::SeparatorText("Developer runtime readout");
        const auto firstProvider = trace.value("nativeHybridReconstructionProvider", std::string{"not recorded"});
        const auto postProvider = trace.value("postHybridReconstructionProvider", std::string{"not recorded"});
        ImGui::Text("First reconstruction: %s", firstProvider.c_str());
        ImGui::Text("Post-Hybrid reconstruction: %s", postProvider.c_str());
        ImGui::Text("Native spatial / temporal dispatches: %llu / %llu",
            static_cast<unsigned long long>(trace.value("nativeHybridReconstructionDispatches", uint64_t{0})),
            static_cast<unsigned long long>(trace.value("nativeHybridTemporalDispatches", uint64_t{0})));
        ImGui::Text("First AMD / native family mask: 0x%X / 0x%X",
            trace.value("firstPassRecordedRRFamilyMask", 0u),
            trace.value("firstPassRecordedNativeFamilyMask", 0u));
        ImGui::Text("Post AMD / native family mask: 0x%X / 0x%X",
            trace.value("postHybridRecordedRRFamilyMask", 0u),
            trace.value("postHybridRecordedNativeFamilyMask", 0u));
        ImGui::TextWrapped("Counts and masks describe CPU-recorded GPU work, not confirmed completed presentation.");
        const auto failure = trace.value("nativeHybridTemporalFailure", std::string{});
        if (!failure.empty())
            ImGui::TextWrapped("Native temporal: %s", failure.c_str());
    }

    if (IsFixedPreset()) {
        if (IsFSRRenderer() && !LegacyHybridPrototypeActive())
            ImGui::TextWrapped("FSR-RR renderer is pending. Fixed-tier controls below describe the safe Shadow Only fallback, NOT the requested neural mode.");
        else
            ImGui::TextWrapped("Fixed tier: lighting and sampling tuning are locked; RR requests remain individually editable.");
        constexpr std::array rrLabels{ "Direct FSR Ray Regeneration",
            "Indirect FSR Ray Regeneration", "HybridGI FSR Ray Regeneration" };
        const auto selectedMode = composition.mode;
        if (selectedMode != CompositorMode::ShadowOnly &&
            selectedMode != CompositorMode::FSRRedstone) {
            ImGui::SeparatorText("Independent RR requests");
            for (uint32_t family = 0u; family < rrLabels.size() && IsFSRRenderer(); ++family) {
                const uint32_t index = static_cast<uint32_t>(selectedMode);
                const bool supported = (index == 1u && family == 2u) ||
                    ((index == 2u || index == 3u) && family < 2u) || index == 4u;
                if (!supported) continue;
                bool requested = composition.fixedRR.Get(selectedMode, family);
                if (ImGui::Checkbox(rrLabels[family], &requested) &&
                    composition.fixedRR.Set(selectedMode, family, requested))
                    OnReconstructionPolicyChanged(selectedMode, family, requested);
            }
            ImGui::TextWrapped("Independent Indirect/HybridGI reconstruction is wired but not yet validated in Skyrim. Native Direct RR remains pending.");
        }
        ImGui::TextWrapped("Direct: %s", DirectTierDescription(composition.mode));
        ImGui::TextWrapped("Indirect: %s", IndirectTierDescription(composition.mode));
        if (!GetTierPolicy(composition.mode).ready)
            ImGui::TextWrapped("Production composition is pending: the effective renderer stays Shadow Only. The diagnostic trace below can exercise transport without changing scene composition.");
        if (composition.mode == CompositorMode::FSRRedstone)
            ImGui::TextWrapped("FSR-REDSTONE unavailable. Its saved request remains intact; Shadow Only is the explicit runtime fallback. Radiance Cache is deferred.");
        if (IsAdvancedTransportTier(composition.mode)) {
            ImGui::TextWrapped("Native DX12 transport no longer requires AMD RR. RR failures use native reconstruction; unrecoverable composition may retain raster.");
            if (ImGui::Checkbox("Publish GPU diagnostic transport signal views", &composition.diagnoseAdvancedTransport))
                OnCompositionPolicyChanged();
            const auto tracerStatus = globals::features::dx12NativeScene.Tracer().GetDiagnostics();
            ImGui::Text("Advanced trace dispatches: %llu | native RR: %llu | final compositions: %llu",
                static_cast<unsigned long long>(tracerStatus.value("advancedTransportDispatches", uint64_t{0})),
                static_cast<unsigned long long>(tracerStatus.value("advancedRRDispatches", uint64_t{0})),
                static_cast<unsigned long long>(tracerStatus.value("advancedComposeSubmissions", uint64_t{0})));
            ImGui::Text("Post-composite HybridGI: %llu",
                static_cast<unsigned long long>(tracerStatus.value("postHybridComposeSubmissions", uint64_t{0})));
            ImGui::Text("Advanced output: %llu presented / %llu raster fallbacks",
                static_cast<unsigned long long>(tracerStatus.value("advancedPresentedFrames", uint64_t{0})),
                static_cast<unsigned long long>(tracerStatus.value("advancedRasterFallbackFrames", uint64_t{0})));
            if (const auto failure = tracerStatus.value("advancedComposeFailure", std::string{}); !failure.empty())
                ImGui::TextWrapped("Advanced native frame fallback: %s", failure.c_str());
        }
        if (composition.mode == CompositorMode::ShadowOnly || composition.mode == CompositorMode::HybridShadowed) {
            const auto preset = GetTierPolicy(composition.mode);
            ImGui::Text("Shadow contribution: %.2f | direct/raster blend: %.2f (fixed)",
                preset.shadowInfluence, preset.directContribution);
        }
    } else {
        ImGui::TextWrapped("Custom profile: these controls are persistent and are not overwritten by switching to a fixed tier.");
        if (IsOpenRT())
            ImGui::TextWrapped("Open RT Custom does not provide HybridGI or FSR-RR; the previous Custom HybridGI setting is retained for migration but not executed.");
        else if (ImGui::Checkbox("Custom: use native DX12 Hybrid-Shadowed GI", &composition.customHybridShadowed))
            OnCompositionPolicyChanged();
        if (ImGui::SliderFloat("Custom shadow influence", &composition.shadowInfluence, 0.0f, 1.0f, "%.2f"))
            OnCompositionPolicyChanged();
        ImGui::SliderFloat("Custom direct/raster blend", &composition.rtDirectContribution, 0.0f, 1.0f, "%.2f");
        if (ImGui::Checkbox("Custom direct native checkerboard (50%)", &composition.directCheckerboard)) {
            ApplySamplingPolicy(composition.directCheckerboard);
            OnCompositionPolicyChanged();
        }
        if (!IsOpenRT()) {
            int giResolution = int(composition.hybridResolution);
            bool giChanged = ImGui::Combo("Custom HybridGI resolution (SSGI sizes)", &giResolution,
                "Full (100% W/H)\0Half (50% W/H)\0Quarter (25% W/H)\0");
            if (giChanged) composition.hybridResolution = uint32_t(giResolution);
            giChanged |= ImGui::Checkbox("Custom HybridGI checkerboard", &composition.hybridCheckerboard);
            if (giChanged) OnCompositionPolicyChanged();
        }
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
    if (composition.enabled && IsOpenRT()) {
        ImGui::TextWrapped("FSR-RR controls are unavailable in Open RT; native reconstruction is authoritative.");
    } else if (composition.enabled && IsFixedPreset()) {
        ImGui::TextWrapped("This is a fixed profile. Direct and indirect settings sections are read-only.");
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
            // Fixed RR policy is managed by setFixedRR, never by the legacy global setting.
            if (requested != static_cast<uint32_t>(CompositorMode::Custom) &&
                args.contains("useFSRRayRegeneration"))
                throw std::invalid_argument("Fixed RR must use setFixedRR with a signal family");
            if (requested == static_cast<uint32_t>(CompositorMode::FSRRedstone))
                throw std::invalid_argument("FSR-REDSTONE unavailable: saved settings remain preserved; runtime uses Shadow Only");
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
                policy.diagnoseAdvancedTransport, policy.useFSRRayRegeneration,
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
            const bool transportDiagnostic = args.value("diagnoseAdvancedTransport", policy.diagnoseAdvancedTransport);
            const bool compositorRR = args.value("useFSRRayRegeneration", policy.useFSRRayRegeneration);
            policy.mode = static_cast<CompositorMode>(requested);
            policy.useFSRRayRegeneration = compositorRR;
            policy.diagnoseAdvancedTransport = transportDiagnostic;
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
                policy.diagnoseAdvancedTransport, policy.useFSRRayRegeneration,
                policy.customHybridShadowed, policy.directCheckerboard,
                policy.hybridResolution, policy.hybridCheckerboard }) {
                if (policy.mode == CompositorMode::Custom)
                    ApplySamplingPolicy(policy.directCheckerboard);
                composer.OnCompositionPolicyChanged();
            }
        });
    registry.RegisterCommand(GetShortName(), "setFixedRR",
        "Save a fixed-tier reconstruction request (family: direct, indirect or hybrid). Does not change tracing.",
        +[](Feature* self, const json& args) {
            auto& composer = *static_cast<RTComposer*>(self);
            const uint32_t modeValue = args.at("mode").get<uint32_t>();
            if (modeValue > 4u)
                throw std::invalid_argument("Fixed RR request requires an available fixed compositor tier (0..4)");
            const auto mode = static_cast<CompositorMode>(modeValue);
            const std::string familyName = args.at("family").get<std::string>();
            const uint32_t family = familyName == "direct" ? 0u :
                familyName == "indirect" ? 1u :
                familyName == "hybrid" ? 2u : 3u;
            const bool enabled = args.at("enabled").get<bool>();
            if (!composer.composition.fixedRR.Set(mode, family, enabled))
                throw std::invalid_argument("This RR signal family is unavailable in the requested tier");
            composer.OnReconstructionPolicyChanged(mode, family, enabled);
            // No tracer, scene, sampling or light snapshot reset for RR policy changes.
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

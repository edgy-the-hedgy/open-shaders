#pragma once
#include "Feature.h"
#include "Features/DX12NativeScene/HybridSamplingPolicy.h"
#include <array>

// Owns UI, settings and control directives; GPU work remains with existing producers.
struct RTComposer final : Feature
{
    // Saved, independent composition policy. Disabled on migration to keep
    // every previously configured direct/SSGI/Hybrid path unchanged.
    enum class CompositorMode : uint32_t {
        ShadowOnly = 0, HybridShadowed = 1, RTExclusiveI = 2,
        RTExclusiveII = 3, RTEnhanced = 4, FSRRedstone = 5, Custom = 6
    };
    struct TierPolicy
    {
        uint32_t directTraceMode; // DXR 6=quarter checkerboard (12.5%), 7=quarter (25%), 5=native checkerboard
        float shadowInfluence;
        float directContribution; // fraction of raster/direct composition, NOT radiance
        bool directSpecular;
        bool directRR;
        bool nativeHybridGI;
        bool nativeHybridRR;
        uint32_t hybridResolution; // 0/1/2 = SSGI full/half/quarter W/H
        bool hybridCheckerboard;
        float firstBounceSurvival;
        float specularBounceFraction;
        float secondBounceSurvival;
        bool ready;
    };
    static constexpr TierPolicy GetTierPolicy(CompositorMode mode)
    {
        switch (mode) {
        case CompositorMode::ShadowOnly:
            return { 6u, .9f, .05f, false, false, false, false, 2u, false, 0.f, 0.f, 0.f, true };
        case CompositorMode::HybridShadowed:
            return { 6u, .9f, .3f, false, false, true, true, 2u, false, 0.f, 0.f, 0.f, true };
        case CompositorMode::RTExclusiveI:
            return { 7u, .8f, .5f, false, true, false, false, 2u, false, .5f, 0.f, 0.f, true };
        case CompositorMode::RTExclusiveII:
            return { 7u, .8f, .5f, false, true, false, false, 2u, false, .5f, .5f, 0.f, true };
        case CompositorMode::RTEnhanced:
            return { 5u, .8f, .5f, true, true, true, true, 2u, true, .5f, .5f, 0.f, true };
        case CompositorMode::FSRRedstone:
            return { 5u, .8f, .5f, true, true, true, true, 2u, true, .66f, .5f, .25f, false };
        case CompositorMode::Custom:
            return { 0u, 0.f, 0.f, true, true, false, false, 0u, false, 0.f, 0.f, 0.f, true };
        }
        return GetTierPolicy(CompositorMode::ShadowOnly);
    }
    // This is the dispatch contract for the native RT/RR pipeline.
    // Never inherit Custom sampling for fixed upper tiers; the DX12 compositor
    // publishes only a completed RR+GI generation with valid resource fences.
    struct TierExecutionContract
    {
        uint32_t directTraceMode = 0u;
        uint32_t indirectTraceMode = 0u;
        uint32_t indirectCandidateRays = 0u; // before Russian roulette
        uint32_t rrSignalFamilies = 0u; // 1=indirect diffuse, 2=indirect specular, 4=AO/SO
        uint32_t hybridResolution = 0u; // SSGI 0 full, 1 half, 2 quarter W/H
        bool hybridCheckerboard = false;
        bool requiresNativeComposition = false;
        bool requiresPerFamilyValidity = false;
        bool needsPostCompositeHybrid = false;
        bool needsSecondBounce = false;
    };
    static constexpr TierExecutionContract GetTierExecutionContract(CompositorMode mode)
    {
        const auto tier = GetTierPolicy(mode);
        switch (mode) {
        case CompositorMode::RTExclusiveI:
            // One candidate ray per traced 2x2 anchor; 50% survives.
            return { 7u, 7u, 1u, 1u, 0u, false, true, true, false, false };
        case CompositorMode::RTExclusiveII:
            return { 7u, 7u, 1u, 3u, 0u, false, true, true, false, false };
        case CompositorMode::RTEnhanced:
            // Direct/indirect: native checkerboard. Follow-up HybridGI:
            // quarter width AND height, itself checkerboarded.
            return { 5u, 5u, 1u, 7u, 2u, true, true, true, true, false };
        case CompositorMode::FSRRedstone:
            return { 5u, 5u, 1u, 7u, 2u, true, true, true, true, true };
        default:
            // The existing two low tiers and Custom keep their live paths.
            return { tier.directTraceMode, 0u, 0u, 0u, tier.hybridResolution,
                tier.hybridCheckerboard, false, false, false, false };
        }
    }
    static constexpr bool IsAdvancedTransportTier(CompositorMode mode)
    {
        return mode >= CompositorMode::RTExclusiveI && mode <= CompositorMode::FSRRedstone;
    }
    static constexpr const char* ModeName(CompositorMode mode)
    {
        switch (mode) {
        case CompositorMode::ShadowOnly: return "shadow-only";
        case CompositorMode::HybridShadowed: return "hybrid-shadowed";
        case CompositorMode::RTExclusiveI: return "rt-exclusive-tier-one";
        case CompositorMode::RTExclusiveII: return "rt-exclusive-tier-two";
        case CompositorMode::RTEnhanced: return "rt-enhanced";
        case CompositorMode::FSRRedstone: return "fsr-redstone";
        case CompositorMode::Custom: return "custom";
        }
        return "invalid";
    }
    // Post-RR-only controls. Filtering never modifies the traced signal or
    // the input sent into Ray Regeneration.
    struct SignalReconstructionSettings
    {
        bool filterEnabled = true;
        float filterRadius = 1.5f;
        float filterStrength = 1.0f;
        float depthSensitivity = 128.0f;
        float normalSensitivity = 32.0f;
        float roughnessSensitivity = 8.0f;
        bool blurEnabled = false;
        float blurRadius = 1.0f;
        float blurStrength = 0.25f;
    };
    struct CompositionSettings
    {
        bool enabled = false;
        // Compositor-owned RR switch. Fixed native sparse tiers require RR;
        // when disabled, preserve the low shadow-only fallback.
        bool useFSRRayRegeneration = true;
        bool diagnoseAdvancedTransport = false; // explicit GPU-only trace diagnostic; never authorizes scene composition
        CompositorMode mode = CompositorMode::ShadowOnly;
        bool customHybridShadowed = false; // saved custom backend choice
        float shadowInfluence = 0.8f;
        float rtDirectContribution = 0.25f; // composition blend; tier's direct radiance still distinct
        float giContribution = 1.0f; // Hybrid-only native GI blend, independent of direct
        float shadowThreshold = 0.25f;
        bool directCheckerboard = false;
        uint32_t hybridResolution = 2u; // SSGI stage 3: quarter width and height
        bool hybridCheckerboard = false;
        // Separate settings for diffuse, specular and AO, respectively.
        std::array<SignalReconstructionSettings, 3> reconstruction{};
    } composition;
    enum class EffectiveMode : uint32_t {
        Legacy = 0, ShadowOnly = 1, HybridShadowed = 2,
        RTExclusive = 3, RTEnhanced = 4
    };
    bool IsFixedPreset() const { return composition.mode != CompositorMode::Custom; }
    bool AdvancedTransportDiagnosticActive() const
    {
        return composition.enabled && composition.diagnoseAdvancedTransport &&
            IsAdvancedTransportTier(composition.mode) && GetTierPolicy(composition.mode).ready;
    }
    bool TierControlsLocked() const { return composition.enabled && IsFixedPreset(); }
    bool WantsNativeHybrid() const
    {
        return composition.mode == CompositorMode::HybridShadowed ||
            (IsAdvancedTransportTier(composition.mode) && GetTierPolicy(composition.mode).ready) ||
            (composition.mode == CompositorMode::Custom && composition.customHybridShadowed);
    }
    static const char* DirectTierDescription(CompositorMode mode);
    static const char* IndirectTierDescription(CompositorMode mode);
    // Advanced modes require a completed scene and packaged DX12/RR shaders.
    // On missing prerequisites the working Shadow Only image stays intact.
    EffectiveMode ResolveEffectiveMode() const
    {
        if (!composition.enabled) return EffectiveMode::Legacy;
        // Persist requested FSR-REDSTONE settings, but never activate its deferred transport.
        if (composition.mode == CompositorMode::FSRRedstone) return EffectiveMode::ShadowOnly;
        if (IsFixedPreset() && composition.mode != CompositorMode::ShadowOnly &&
            !composition.useFSRRayRegeneration) return EffectiveMode::ShadowOnly;
        if (IsAdvancedTransportTier(composition.mode) &&
            GetTierPolicy(composition.mode).ready && DX12HybridReady())
            return composition.mode <= CompositorMode::RTExclusiveII ?
                EffectiveMode::RTExclusive : EffectiveMode::RTEnhanced;
        if (WantsNativeHybrid() && DX12HybridReady())
            return EffectiveMode::HybridShadowed;
        return EffectiveMode::ShadowOnly;
    }
    // Custom Shadow Only retains the DX11 composition exception, so the
    // existing DXR indirect producer can be selected there if desired.
    // Fixed tiers and native HybridGI continue to exclude the old producer.
    bool BlocksRTIndirect() const
    {
        return composition.enabled &&
            !(composition.mode == CompositorMode::Custom && !composition.customHybridShadowed);
    }
    bool ShadowOnlyActive() const { return ResolveEffectiveMode() == EffectiveMode::ShadowOnly; }
    bool DX12HybridActive() const
    {
        const auto mode = ResolveEffectiveMode();
        return mode == EffectiveMode::HybridShadowed ||
            mode == EffectiveMode::RTExclusive || mode == EffectiveMode::RTEnhanced;
    }
    bool AdvancedActive() const
    {
        const auto mode = ResolveEffectiveMode();
        return mode == EffectiveMode::RTExclusive || mode == EffectiveMode::RTEnhanced;
    }
    // The native producer is eligible when its shaders and retained scene are ready.
    // Per-frame RR/compose failures fail open to unchanged raster, never DX11 HybridGI.
    bool DX12HybridReady() const;
    void OnCompositionPolicyChanged();
    json GetDiagnostics() override;
    std::string GetName() override { return "RTComposer"; }
    std::string GetShortName() override { return "RTComposer"; }
    std::string GetDisplayName() override { return "RT Composer"; }
    std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
    bool IsCore() const override { return true; }
    bool IsAlwaysEnabled() const override { return true; }
    bool SupportsVR() override { return true; }
    bool UsesMainSettings() const override { return true; }
    std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;
    void ImportLegacySettings(json& root) override;
    void LoadSettings(json& value) override;
    void SaveSettings(json& value) override;
    void RestoreDefaultSettings() override;
    void DrawSettings() override;
    void RegisterUxActions() override;
    static void MigrateLegacySettings(json& root);
    static void MigrateChangedLegacySettings(const json& before, json& after);
};

static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::ShadowOnly).directTraceMode == 6u);
static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::HybridShadowed).directTraceMode == 6u);
static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::HybridShadowed).shadowInfluence == .9f);
static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::ShadowOnly).directContribution == .05f);
static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::HybridShadowed).directContribution == .3f);
static_assert(!RTComposer::GetTierPolicy(RTComposer::CompositorMode::ShadowOnly).directRR);
static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::HybridShadowed).hybridResolution == 2u);
static_assert(!RTComposer::GetTierPolicy(RTComposer::CompositorMode::FSRRedstone).ready);
static_assert(RTComposer::GetTierExecutionContract(RTComposer::CompositorMode::RTExclusiveI).directTraceMode == 7u);
static_assert(RTComposer::GetTierExecutionContract(RTComposer::CompositorMode::RTExclusiveI).rrSignalFamilies == 1u);
static_assert(RTComposer::GetTierExecutionContract(RTComposer::CompositorMode::RTExclusiveII).rrSignalFamilies == 3u);
static_assert(RTComposer::GetTierExecutionContract(RTComposer::CompositorMode::RTEnhanced).directTraceMode == 5u);
static_assert(RTComposer::GetTierExecutionContract(RTComposer::CompositorMode::RTEnhanced).hybridCheckerboard);
static_assert(RTComposer::GetTierExecutionContract(RTComposer::CompositorMode::FSRRedstone).needsSecondBounce);
static_assert(!RTComposer::GetTierExecutionContract(RTComposer::CompositorMode::ShadowOnly).requiresNativeComposition);
static_assert(!RTComposer::GetTierExecutionContract(RTComposer::CompositorMode::Custom).requiresPerFamilyValidity);
static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::RTExclusiveI).firstBounceSurvival == 0.5f);
static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::RTExclusiveI).specularBounceFraction == 0.0f);
static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::RTExclusiveII).firstBounceSurvival == 0.5f);
static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::RTExclusiveII).specularBounceFraction == 0.5f);
static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::RTEnhanced).directTraceMode == 5u);
static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::FSRRedstone).firstBounceSurvival == 0.66f);
static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::FSRRedstone).secondBounceSurvival == 0.25f);

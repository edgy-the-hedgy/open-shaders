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
        uint32_t directTraceMode; // DXR mode 7 = quarter pixels, 5 = native checkerboard
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
            return { 7u, .9f, .05f, false, false, false, false, 2u, false, 0.f, 0.f, 0.f, true };
        case CompositorMode::HybridShadowed:
            return { 7u, .9f, .3f, false, false, true, true, 2u, false, 0.f, 0.f, 0.f, true };
        case CompositorMode::RTExclusiveI:
            return { 7u, .8f, .5f, false, true, false, false, 2u, false, .5f, 0.f, 0.f, false };
        case CompositorMode::RTExclusiveII:
            return { 7u, .8f, .5f, false, true, false, false, 2u, false, .5f, .5f, 0.f, false };
        case CompositorMode::RTEnhanced:
            return { 5u, .8f, .5f, true, true, true, true, 2u, true, .5f, .5f, 0.f, false };
        case CompositorMode::FSRRedstone:
            return { 5u, .8f, .5f, true, true, true, true, 2u, true, .66f, .5f, .25f, false };
        case CompositorMode::Custom:
            return { 0u, 0.f, 0.f, true, true, false, false, 0u, false, 0.f, 0.f, 0.f, true };
        }
        return GetTierPolicy(CompositorMode::ShadowOnly);
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
    enum class EffectiveMode : uint32_t { Legacy = 0, ShadowOnly = 1, HybridShadowed = 2 };
    bool IsFixedPreset() const { return composition.mode != CompositorMode::Custom; }
    bool TierControlsLocked() const { return composition.enabled && IsFixedPreset(); }
    bool WantsNativeHybrid() const
    {
        return composition.mode == CompositorMode::HybridShadowed ||
            (composition.mode == CompositorMode::Custom && composition.customHybridShadowed);
    }
    static const char* DirectTierDescription(CompositorMode mode);
    static const char* IndirectTierDescription(CompositorMode mode);
    // Never expose unimplemented RT-exclusive / enhanced transport as a live
    // production path. Their selected presets remain visible in diagnostics,
    // but the effective renderer falls back to the supported Shadow Only mode.
    EffectiveMode ResolveEffectiveMode() const
    {
        if (!composition.enabled) return EffectiveMode::Legacy;
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
    bool DX12HybridActive() const { return ResolveEffectiveMode() == EffectiveMode::HybridShadowed; }
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

static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::ShadowOnly).directTraceMode == 7u);
static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::HybridShadowed).shadowInfluence == .9f);
static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::ShadowOnly).directContribution == .05f);
static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::HybridShadowed).directContribution == .3f);
static_assert(!RTComposer::GetTierPolicy(RTComposer::CompositorMode::ShadowOnly).directRR);
static_assert(RTComposer::GetTierPolicy(RTComposer::CompositorMode::HybridShadowed).hybridResolution == 2u);
static_assert(!RTComposer::GetTierPolicy(RTComposer::CompositorMode::FSRRedstone).ready);

#pragma once
#include "Feature.h"

// Owns UI, settings and control directives; GPU work remains with existing producers.
struct RTComposer final : Feature
{
    // Saved, independent composition policy. Disabled on migration to keep
    // every previously configured direct/SSGI/Hybrid path unchanged.
    enum class CompositorMode : uint32_t { ShadowOnly = 0, HybridShadowed = 1 };
    struct CompositionSettings
    {
        bool enabled = false;
        CompositorMode mode = CompositorMode::ShadowOnly;
        float shadowInfluence = 1.0f;
        float rtDirectContribution = 0.0f;
        float shadowThreshold = 0.25f;
    } composition;
    enum class EffectiveMode : uint32_t { Legacy = 0, ShadowOnly = 1, HybridShadowed = 2 };
    // The saved mode and the actually dispatched mode are intentionally distinct:
    // until the native DX12 GI pipeline is ready, Hybrid-Shadowed degrades to
    // Shadow Only rather than activating the legacy DX11 HybridGI estimator.
    EffectiveMode ResolveEffectiveMode() const
    {
        if (!composition.enabled) return EffectiveMode::Legacy;
        if (composition.mode == CompositorMode::HybridShadowed && DX12HybridReady())
            return EffectiveMode::HybridShadowed;
        return EffectiveMode::ShadowOnly;
    }
    bool BlocksRTIndirect() const { return composition.enabled; }
    bool ShadowOnlyActive() const { return ResolveEffectiveMode() == EffectiveMode::ShadowOnly; }
    bool DX12HybridActive() const { return ResolveEffectiveMode() == EffectiveMode::HybridShadowed; }
    // Future Stage 5 implementation will replace this readiness gate with
    // verified live DX12 estimator/denoiser/composer availability.
    bool DX12HybridReady() const { return false; }
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

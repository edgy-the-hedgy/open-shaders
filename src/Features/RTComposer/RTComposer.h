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
    bool ShadowOnlyActive() const { return composition.enabled && composition.mode == CompositorMode::ShadowOnly; }
    // Hybrid-Shadowed remains inactive until its DX12 GI producer is implemented.
    bool DX12HybridActive() const { return false; }
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

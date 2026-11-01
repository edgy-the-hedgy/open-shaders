#pragma once
#include "Feature.h"

// Owns UI, settings and control directives; GPU work remains with existing producers.
struct RTComposer final : Feature
{
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

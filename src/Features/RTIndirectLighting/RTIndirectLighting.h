#pragma once
#include "Feature.h"

/** @brief Owns the indirect-lighting method and its direct-lighting dependency. */
struct RTIndirectLighting final : Feature
{
	struct Settings
	{
		bool UseRTExclusive = false;
		uint32_t IndirectRays = 1;
		float IndirectStrength = 1.0f;
		bool ConnectHybrid = true;
		uint32_t HybridResolution = 2; // 0=full, 1=half, 2=quarter in each dimension
        bool HybridCheckerboard = false; // independent of Direct sampling
        float HybridContribution = 1.0f;
        bool UseHybridRayRegeneration = true; // RR never gates raw HybridGI
		uint32_t RTTraceResolution = 0;  // 0=full, 1=half, 2=quarter, 3=two-thirds, 4=one-third (linear sampling)
	} settings;

	std::string GetName() override { return "RTIndirectLighting"; }
	std::string GetShortName() override { return "RTIndirectLighting"; }
	std::string GetDisplayName() override { return "RT Indirect Lighting / AO"; }
	std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	bool IsCore() const override { return true; }
	bool UsesMainSettings() const override { return true; }
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;
	void ImportLegacySettings(json& root) override;
	void LoadSettings(json& value) override;
	void SaveSettings(json& value) override;
	void RestoreDefaultSettings() override;
	void DrawSettings() override;
	void RegisterUxActions() override;
	json GetDiagnostics() override;
	int GetIndirectLightingPriority() const override;
	/** @brief True only when the RT-direct producer is registered and enabled. */
	bool UsesRTExclusive() const;
	/** @brief Switches to the raymarched Hybrid mode, retiring exclusive trace history. */
	void SelectHybrid();
	void SelectRTExclusive();
	void DrawUnifiedDebugSettings();
	bool DebugAtlasActive() const { return debugView == 13u; }
private:
	uint32_t debugView = 0;
	float debugExposure = 0.0f;
};

#pragma once
#include "Feature.h"

/** @brief Owns the indirect-lighting method and its direct-lighting dependency. */
struct RTIndirectLighting final : Feature
{
	struct Settings
	{
		bool UseRTExclusive = false;
		uint32_t IndirectRays = 1;
		uint32_t RTBounces = 2;
		bool UseIndirectRayRegeneration = true; // 0=disabled, 1=first indirect hit, 2=terminal second bounce
		float IndirectStrength = 1.0f;
		bool ConnectHybrid = true;
		uint32_t HybridResolution = 3; // 3=inherit existing RR resolution; 0/1/2=full/half/quarter
		uint32_t RTTraceResolution = 0; // legacy JSON migration input only
		uint32_t RTSamplingMode = 0;    // authoritative eight-mode native pixel density
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

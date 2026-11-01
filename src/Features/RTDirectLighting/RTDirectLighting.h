#pragma once
#include "Features/OverlayFeature.h"
#include <optional>

struct RTDirectLighting final : OverlayFeature
{
	struct Settings
	{
		float RedstoneBlend = 1.0f;
		bool UsePreparedDirectLighting = false;
		bool UseAllPreparedLights = true;
		bool ConnectIndirectLighting = true;
		bool UseDirectRayRegeneration = true;
		bool UseDXRIndirect = false;
		uint32_t IndirectRays = 1;
		float DirectStrength = 1.0f;
		float DiffuseStrength = 1.0f;
		float SpecularStrength = 1.0f;
		float EmissionStrength = 1.0f;
		float IndirectStrength = 1.0f;
		uint32_t SelectedDirectLight = 0;
	} settings;
	std::string GetName() override { return "RTDirectLighting"; }
	std::string GetShortName() override { return "RTDirectLighting"; }
	std::string GetDisplayName() override { return "RT Direct Lighting"; }
	std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	bool IsCore() const override { return true; }
	bool UsesMainSettings() const override { return true; }
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;
	void ImportLegacySettings(json& root) override;
	void LoadSettings(json& value) override;
	void SaveSettings(json& value) override;
	void RestoreDefaultSettings() override;
	void DrawSettings() override;
	void DrawDirectLightingSettings();
	void DrawIndirectLightingSettings();
	void DrawRayRegenerationSettings();
	void ClearRRDebugOverlay();
	void DrawOverlay() override;
	bool IsOverlayVisible() const override;
	void OnAfterDeferredLighting(ID3D11UnorderedAccessView* target, const std::array<uint32_t, 2>& size) override;
	void OnSceneTransitionReset(bool) override;
	void OnRuntimeDisabled() override;
	void ClearShaderCache() override;
	int GetIndirectLightingPriority() const override;
	bool UsesDXRIndirect() const { return settings.UsePreparedDirectLighting && settings.UseDXRIndirect; }
	bool WantsComposite() const { return settings.UsePreparedDirectLighting; }
	json GetDiagnostics() override;
	void RegisterUxActions() override;
private:
	std::optional<uint32_t> lastFrame;
};

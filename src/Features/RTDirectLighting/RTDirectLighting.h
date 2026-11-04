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
		bool UseDirectRayRegeneration = true;
		float DirectStrength = 1.0f;
		float DiffuseStrength = 1.0f;
		float SpecularStrength = 1.0f;
		float EmissionStrength = 1.0f;
		// Runtime tuning of the prepared point-light cutoff (inverse-square intensity is unchanged).
		float PointLightRadiusMultiplier = 1.0f;
		float PointLightFalloffFraction = 0.1f;
		float PointLightFalloffExponent = 1.0f;
		// Combined temporal sparse sampling and finite-emitter visibility (optional experimental path).
		bool StochasticSampling = false;
		float EmitterRadiusMultiplier = 1.0f;
		uint32_t SelectedDirectLight = 0;
		uint32_t DirectTraceResolution = 0;  // 0=full, 1=half, 2=quarter, 3=two-thirds, 4=one-third (linear sampling)
		bool RRMaterialIsolation = false;
		float RRGlossyRoughness = 0.2f;
		uint32_t DirectRRResolution = 3;  // 3 inherits existing shared RR resolution
        // Direct producer-owned FSR signal conditioning and composition.
        bool FSREarlyShadowMask = false;
        bool ShadowContactAA = true;
        float ShadowInfluence = 0.9f; // raster visibility attenuation, not RT radiance
        float FSRDirectBlend = 1.0f;
        bool FSRClampRadiance = false;
        float FSRRadianceClamp = 16384.0f;
		uint32_t NormalInfluence = 2;  // 0=legacy flat, 1=geometric, 2=full material/True PBR
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
	void DrawRayRegenerationSettings();
	void ClearRRDebugOverlay();
	void DrawOverlay() override;
	bool IsOverlayVisible() const override;
	void OnAfterDeferredLighting(ID3D11UnorderedAccessView* target, const std::array<uint32_t, 2>& size) override;
	void OnSceneTransitionReset(bool) override;
	void OnRuntimeDisabled() override;
	void ClearShaderCache() override;
	bool WantsComposite() const { return settings.UsePreparedDirectLighting; }
	json GetDiagnostics() override;
	void RegisterUxActions() override;
private:
	std::optional<uint32_t> lastFrame;
};

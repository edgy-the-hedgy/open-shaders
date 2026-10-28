#pragma once

#include "Features/OverlayFeature.h"
#include "Scene.h"
#include "GeometryBridge.h"
#include "AccelerationScene.h"
#include "DirectionalTrace.h"

/** @brief Independently owns Redstone shadow acquisition, settings and diagnostics. */
struct RedstoneShadows final : OverlayFeature
{
	struct Settings
	{
		bool AcquireScene = false;
		uint32_t NodesPerFrame = 2048;
		uint32_t MaxInstances = 8192;
		uint32_t BufferBudgetMB = 128;
	} settings;

	std::string GetName() override { return "RedstoneShadows"; }
	std::string GetShortName() override { return "RedstoneShadows"; }
	std::string GetDisplayName() override { return "RedstoneShadows"; }
	std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	bool IsCore() const override { return true; }
	bool UsesMainSettings() const override { return true; }
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;
	/** @brief Loads bounded independent acquisition settings. */
	void LoadSettings(json& a_json) override;
	/** @brief Saves only feature-owned settings. */
	void SaveSettings(json& a_json) override;
	/** @brief Disables acquisition and invalidates scene records. */
	void RestoreDefaultSettings() override;
	/** @brief Draws acquisition controls and explicit production/fallback status. */
	void DrawSettings() override;
	/** @brief Probes DXR separately from the unchanged RR/session gate. */
	void SetupResources() override;
	/** @brief Advances owned scene acquisition without changing shadow ownership. */
	void EarlyPrepass() override;
	/** @brief Invalidates observations across scene transitions. */
	void OnSceneTransitionReset(bool a_open) override;
	/** @brief Invalidates scene identity after loading a save. */
	void GameLoaded() override { resetRequested = true; }
	/** @brief Invalidates scene snapshots when shader-side deformation may change. */
	void ClearShaderCache() override { resetRequested = true; geometryBridge.ReloadShader(); directional.ReloadShaders(); }
	/** @brief Traces final opaque receiver depth after native world rendering. */
	void OnBeforePostProcessing(RE::RENDER_TARGET) override;
	/** @brief Displays completed raw visibility with an explicit partial-coverage label. */
	void DrawOverlay() override;
	bool IsOverlayVisible() const override { return previewDirectional && settings.AcquireScene; }
	/** @brief Exposes unsaved directional tracing and preview switches. */
	json GetRuntimeFlags() override;
	bool SetRuntimeFlag(std::string_view a_name, bool a_value) override;
	/** @brief Reports acquisition evidence separately from traversal and consumption. */
	json GetDiagnostics() override;
	/** @brief Registers acquisition controls and read-only shadow status. */
	void RegisterUxActions() override;

private:
	static constexpr uint32_t kBackendRetryFrames = 60;
	RedstoneShadow::Scene scene;
	RedstoneShadow::GeometryBridge geometryBridge;
	RedstoneShadow::AccelerationScene acceleration;
	RedstoneShadow::DirectionalTrace directional;
	bool traceDirectional = false, previewDirectional = false;
	std::optional<uint32_t> lastTraceFrame;
	bool ready = false, dxrCapable = false, rrEligible = false;
	bool resetRequested = true, transitionOpen = false;
	std::optional<uint32_t> previousFrame;
	double lastCPUms = 0, directionalCPUms = 0;
	std::optional<uint32_t> directionalCPUFrame;
	std::string unavailable = "Graphics backend not initialized";
};

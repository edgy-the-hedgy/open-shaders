#pragma once

#include "Features/OverlayFeature.h"
#include "Scene.h"
#include "GeometryBridge.h"
#include "AccelerationScene.h"
#include "PreparedScene.h"
#include "DirectionalTrace.h"
#include <atomic>

/** @brief Independently owns Redstone shadow acquisition, settings and diagnostics. */
struct RedstoneShadows final : OverlayFeature
{
	struct Settings
	{
		bool AcquireScene = false;
		bool UsePreparedScene = false;
		bool PreviewPreparedScene = false;
		uint32_t PreparedLight = 0;
		uint32_t NodesPerFrame = 2048;
		uint32_t MaxInstances = CasterObservation::kCapacity;
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
	/** @brief Draws copied caster bounds and diagnostic pipeline progress. */
	void DrawOverlay() override;
	/** @brief Enables the temporary flat-screen capture diagnostic overlay. */
	bool IsOverlayVisible() const override;
	/** @brief Probes DXR separately from the unchanged RR/session gate. */
	void SetupResources() override;
	/** @brief Advances owned scene acquisition without changing shadow ownership. */
	void EarlyPrepass() override;
	/** @brief Traces the prepared baseline scene only after native world depth is complete. */
	void OnWorldRenderEnd(RE::RENDER_TARGET a_target) override;
	/** @brief Disarms native observation before the feature stops receiving render callbacks. */
	void OnRuntimeDisabled() override;
	/** @brief Invalidates observations across scene transitions. */
	void OnSceneTransitionReset(bool a_open) override;
	/** @brief Invalidates scene identity after loading a save. */
	void GameLoaded() override { resetRequested = true; }
	/** @brief Invalidates scene snapshots when shader-side deformation may change. */
	void ClearShaderCache() override { resetRequested = true; geometryBridge.ReloadShader(); preparedTrace.ReloadShaders(); }
	/** @brief Reports unavailable directional flags for the local-light observation scope. */
	json GetRuntimeFlags() override;
	bool SetRuntimeFlag(std::string_view a_name, bool a_value) override;
	/** @brief Reports acquisition evidence separately from traversal and consumption. */
	json GetDiagnostics() override;
	/** @brief Registers acquisition controls and read-only shadow status. */
	void RegisterUxActions() override;

private:
	RedstoneShadow::PreparedScene preparedScene;
	RedstoneShadow::AccelerationWorker preparedAcceleration;
	RedstoneShadow::DirectionalTrace preparedTrace;
	uint32_t preparedCellID = 0;
	uint64_t preparedRevision = 1;
	bool preparedSubmitted = false;
	bool preparedActive = false;
	std::optional<uint32_t> preparedTraceFrame;
	/** @brief Identifies the loaded cell and advances offline upload/AS/preview without scene traversal. */
	void AdvancePreparedScene(uint32_t a_frame, uint64_t a_budgetBytes);
	/** @brief Invalidates prepared publication and retains in-flight work for nonblocking retirement. */
	void ResetPreparedScene();
	static constexpr uint32_t kBackendRetryFrames = 60;
	RedstoneShadow::Scene scene;
	RedstoneShadow::GeometryBridge geometryBridge;
	RedstoneShadow::AccelerationWorker acceleration;
	RedstoneShadow::AccelerationScene::GeometryMap preparedGeometry;
	uint64_t observedEpoch = 0, preparingBatch = 0, workerBatch = 0, exportProgress = UINT64_MAX, discardedBatches = 0;
	bool acquisitionActive = false;
	bool ready = false, dxrCapable = false, rrEligible = false;
	bool resetRequested = true, transitionOpen = false;
	std::optional<uint32_t> previousFrame;
	double lastCPUms = 0;
	std::string unavailable = "Graphics backend not initialized";
	std::string lastDiscardReason;
	uint64_t lastDiscardBatch = 0;
	uint64_t preparingEpochDiscards = 0;
	std::atomic<bool> captureDebug{ false };
	std::array<float4, CasterObservation::kCapacity> debugBounds{};
	uint32_t debugBoundCount = 0;
	CasterObservation::Scope debugScope;
	json debugStatus;
	/** @brief Copies bounded scalar diagnostics without retaining geometry resources. */
	void UpdateCaptureDebug();
	/** @brief Prepares one retained batch on export progress and hands it to the AS worker. */
	void AdvanceAcquisition(const GraphicsBackend& a_backend, uint64_t a_frame, uint64_t a_budgetBytes);
};

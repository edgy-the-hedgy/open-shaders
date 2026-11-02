#pragma once

#include "Features/OverlayFeature.h"
#include "Features/DX12NativeScene/Scene.h"
#include "Features/DX12NativeScene/GeometryBridge.h"
#include "Features/DX12NativeScene/AccelerationScene.h"
#include "Features/DX12NativeScene/PreparedScene.h"
#include "Features/DX12NativeScene/DirectionalTrace.h"
#include <atomic>
#include <mutex>
#include <dxgi1_4.h>

/** @brief Independently owns Redstone shadow acquisition, settings and diagnostics. */
struct DX12NativeScene final : OverlayFeature
{
	struct Settings
	{
		//yes this slider should be moved to FSR-RayRegeneration but until settings catalogue is optimized i will be staying here
		//peace and love, egdy
		bool AcquireScene = false;
		bool UsePreparedScene = false;
		bool AutoPrepareGuestRoom = false; // legacy setting; offline atlas preparation only
		uint32_t PreparationQuality = 0; // Q0 base cache; higher detail is runtime progression, not offline tiers.
		bool PreviewPreparedScene = false;
		bool UsePreparedMaterials = false;
		uint32_t PreparedMaterialView = 0;
		uint32_t MaterialTextureBudgetMB = 512;
		// 0 = DXGI local-memory-aware auto budget; otherwise an explicit MiB cap.
		uint32_t DirectLightingBudgetMB = 0;
		uint32_t PreviewLight = 0;
		bool PreviewAllLights = true;
		float PreviewDirectStrength = 1.0f;
		float PreviewDiffuseStrength = 1.0f;
		float PreviewSpecularStrength = 1.0f;
		float PreviewEmissionStrength = 1.0f;
		uint32_t NodesPerFrame = 2048;
		uint32_t MaxInstances = CasterObservation::kCapacity;
		uint32_t BufferBudgetMB = 128;
	} settings;

	std::string GetName() override { return "DX12NativeScene"; }
	std::string GetShortName() override { return "DX12NativeScene"; }
	std::string GetDisplayName() override { return "DX12 Native Scene"; }
	std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	bool IsCore() const override { return true; }
	bool UsesMainSettings() const override { return true; }
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;
	/** @brief Loads bounded independent acquisition settings. */
	void ImportLegacySettings(json& root) override;
	void LoadSettings(json& a_json) override;
	/** @brief Saves only feature-owned settings. */
	void SaveSettings(json& a_json) override;
	/** @brief Disables acquisition and invalidates scene records. */
	void RestoreDefaultSettings() override;
	/** @brief Retained completed native-scene publication, with no borrowed scene pointers. */
	struct PublishedScene
	{
		std::shared_ptr<const DX12Native::PreparedCell> cell;
		std::shared_ptr<const DX12Native::TopLevel> acceleration;
		std::vector<std::array<uint32_t, 2>> lightReferences;
		std::array<uint32_t, 128> roomReferences{};
		uint32_t cellID = 0;
		uint64_t revision = 0;
	};
	/** @brief Returns the current cell's completed scene, or no scene if disabled/stale. */
	std::optional<PublishedScene> GetPublishedScene() const;
	/** @brief Scene-only request; used by the future lighting feature's compatibility controls. */
	void RequestPreparedScene();
	/** @brief Shared tracer remains owned by the native-scene lifetime. */
	DX12Native::DirectionalTrace& Tracer() { return preparedTrace; }
	const DX12Native::DirectionalTrace& Tracer() const { return preparedTrace; }
	json GetPreparedDiagnostics() const { return preparedScene.GetDiagnostics(); }
	uint64_t GetLightingBudgetBytes() const;
    /** @brief OS-reported local memory budget/headroom, including all allocations. */
    json GetLightingBudgetDiagnostics() const;

	/** @brief Draws acquisition controls and explicit production/fallback status. */
    // Query the adapter associated with the active DX12 device, not the
    // primary display adapter. Cached to avoid factory enumeration per frame.
    mutable std::mutex lightingBudgetMutex;
    mutable winrt::com_ptr<IDXGIAdapter3> lightingBudgetAdapter;
    mutable LUID lightingBudgetAdapterLuid{};
    mutable DXGI_QUERY_VIDEO_MEMORY_INFO lastLightingMemoryInfo{};
    mutable bool lightingMemoryInfoValid = false;
    mutable uint64_t lastEffectiveLightingBudgetBytes = 0;
    static constexpr uint32_t kMaxLightingBudgetMiB = 32768;

	void DrawSettings() override;
	/** @brief Draws exclusive producer selection and settings for the selected indirect signal. */
	/** @brief Draws reconstruction controls, contexts and buffers inside Upscaling's RR section. */
	/** @brief Clears prepared full-screen debugging when another RR viewer takes ownership. */
	/** @brief Draws copied caster bounds and diagnostic pipeline progress. */
	void DrawOverlay() override;
	/** @brief Enables the temporary flat-screen capture diagnostic overlay. */
	bool IsOverlayVisible() const override;
	/** @brief Probes DXR separately from the unchanged RR/session gate. */
	void SetupResources() override;
	/** @brief Advances owned scene acquisition without changing shadow ownership. */
	void EarlyPrepass() override;
	/** @brief Traces previews or direct scene lighting with the opaque world camera and matching depth. */
	void OnAfterDeferredLighting(ID3D11UnorderedAccessView* a_target, const std::array<uint32_t, 2>& a_size) override;
	/** @brief Requests receiver-room storage only for the active eligible prepared-scene session. */
	bool WantsOpaqueRoomMetadata() const override;
	/** @brief Claims exclusive indirect ownership even when a requested DXR scene is unavailable. */
	/** @brief Reports the selected indirect producer independently of resource availability. */
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
	void DrawNativeSceneSettings();
	DX12Native::PreparedScene preparedScene;
	DX12Native::AccelerationWorker preparedAcceleration;
	DX12Native::DirectionalTrace preparedTrace;
	// Read on the existing native-scene render-thread handoff, never on UI thread.
	// These are identities only: character mesh/skinning registration is a later stage.
	std::atomic<uint32_t> runtimeCellID{ 0 }, runtimeWorldspaceID{ 0 }, runtimeCharacterID{ 0 };
	std::atomic<bool> runtimeExterior{ false };
	uint32_t preparedCellID = 0;
	uint64_t preparedRevision = 1;
	bool preparedSubmitted = false;
	bool preparedActive = false;
	std::optional<uint32_t> preparedTraceFrame;
	std::vector<std::array<uint32_t, 2>> preparedLightReferences;
	std::array<uint32_t, 128> preparedRoomReferences{};
	/** @brief Identifies the loaded cell and advances offline upload/AS/preview without scene traversal. */
	void AdvancePreparedScene(uint32_t a_frame, uint64_t a_budgetBytes);
	/** @brief Invalidates prepared publication and retains in-flight work for nonblocking retirement. */
	void ResetPreparedScene();
	static constexpr uint32_t kBackendRetryFrames = 60;
	DX12Native::Scene scene;
	DX12Native::GeometryBridge geometryBridge;
	DX12Native::AccelerationWorker acceleration;
	DX12Native::AccelerationScene::GeometryMap preparedGeometry;
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

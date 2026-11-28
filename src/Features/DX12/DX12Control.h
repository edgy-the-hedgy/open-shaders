#pragma once

#include "DX12ControlMode.h"
#include "Feature.h"
#include "GraphicsBackend.h"

#include <array>
#include <atomic>

/** @brief Session-wide opt-in for DX12 Resources and future native features. */
struct DX12Control final : Feature
{
	struct Settings
	{
		bool Enabled = true;
		int Mode = static_cast<int>(DX12::ControlMode::FullRT);
		bool NativeScreenSpaceReflections = true;
		float NativeWaterReflectionRadius = 0.05f;
		int NativeCubemapCaptureInterval = 6;
	};

	std::string GetName() override { return "DX12"; }
	std::string GetShortName() override { return "DX12"; }
	std::string_view GetCategory() const override { return FeatureCategories::kDX12; }
	bool IsCore() const override { return true; }
	// The master checkbox is the only DX12 activation control; do not offer a
	// separate Disable-at-Boot switch that can silently override it.
	bool IsAlwaysEnabled() const override { return true; }
	bool SupportsVR() override { return false; }
	/** @brief Whether native work may be submitted this frame. */
	bool IsActive() const { return loaded && bootActive.load(std::memory_order_acquire) && !GraphicsBackend::NativeDeviceLost(); }
	bool IsShadowOffloadMode() const { return GetMode() == DX12::ControlMode::ShadowOffload; }
	bool IsFullRTMode() const { return GetMode() == DX12::ControlMode::FullRT; }
	bool IsFullNativeMode() const { return GetMode() == DX12::ControlMode::FullNative; }
	/** @brief Material capture is admitted only for an active Full Native session. */
	bool WantsMaterialPreparation() const { return IsActive() && UsesExclusiveMaterials(); }
	/** @brief Session authority is independent of runtime DX12 availability. */
	DX12::FeatureBackend GetFeatureBackend() const { return featureBackend.load(std::memory_order_acquire); }
	bool UsesExclusiveMaterials() const { return GetFeatureBackend() == DX12::FeatureBackend::FullNative; }
	static std::array<Feature*, 4> NativeCounterparts();
	static bool IsNativeCounterpart(const Feature* feature);
	static DX12::CounterpartKind ClassifyCounterpart(const Feature* feature);
	DX12::ExecutionRole GetExecutionRole(const Feature* feature) const;
	// Selects the single radiance acquisition owner, not the fallback renderer.
	bool UsesLegacyReflectionCapture() const { return GetFeatureBackend() == DX12::FeatureBackend::Baseline; }
	ID3D11ShaderResourceView* GetFallbackSkyRadiance(ID3D11ShaderResourceView* legacyView) const;
	json GetDiagnostics() override;
	void InitializeFeatureBackend();
	void InstallMaterialBackendHooks();
	void DrawSettings() override;
	void RegisterUxActions() override;
	bool NativeSSREnabled() const { return WantsMaterialPreparation() && nativeSSR.load(std::memory_order_relaxed); }
	float NativeWaterReflectionRadius() const { return waterReflectionRadius.load(std::memory_order_relaxed); }
	uint32_t NativeCubemapCaptureInterval() const { return captureInterval.load(std::memory_order_relaxed); }
	void MigrateLegacySettings(json& root) const override;
	void LoadSettings(json& input) override;
	void SaveSettings(json& output) override;
	void RestoreDefaultSettings() override;
	void OnRuntimeDisabled() override;

private:
	std::atomic<bool> bootActive{ false };
	std::atomic<DX12::FeatureBackend> featureBackend{ DX12::FeatureBackend::Baseline };
	bool authorityCaptured = false;
	bool bootCaptured = false;
	DX12::ControlMode GetMode() const;
	void DrawSimpleSettings(bool developerMode);
	void RequestMode(DX12::ControlMode mode);
	mutable std::atomic<int> selectedMode{ Settings{}.Mode };
	std::atomic<bool> nativeSSR{ true };
	std::atomic<float> waterReflectionRadius{ Settings{}.NativeWaterReflectionRadius };
	std::atomic<uint32_t> captureInterval{ uint32_t(Settings{}.NativeCubemapCaptureInterval) };
	Settings settings;
};

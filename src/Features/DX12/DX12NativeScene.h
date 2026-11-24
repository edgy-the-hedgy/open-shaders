#pragma once

#include "DX12NativeAcceleration.h"
#include "DX12NativeRasterCapture.h"
#include "DX12NativeRuntimeCapture.h"
#include "DX12NativeLighting.h"
#include "DX12NativeComposite.h"
#include "DX12NativeScreenProbe.h"
#include "Feature.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/** @brief Independent early opaque raster capture for native DX12. */
struct DX12NativeScene final : Feature
{
	std::string GetName() override { return "DX12 Native Scene"; }
	std::string GetShortName() override { return "DX12NativeScene"; }
	std::string_view GetCategory() const override { return FeatureCategories::kDX12; }
	bool IsCore() const override { return true; }
	bool IsAlwaysEnabled() const override { return true; }
	bool IsInMenu() const override { return false; }
	bool IsInFeatureList() const override { return false; }
	bool SupportsVR() override { return false; }

	void DrawSettings() override;
	/** @brief Draw available Full Native lighting controls. */
	void DrawLightingSettings(bool developer);
	/** @brief Draw available Full Native material controls. */
	void DrawMaterialSettings(bool developer);
	void DrawDevStats();
	bool WantsRenderPassHook() const override { return true; }
	std::function<void()> OnRenderPassBegin(const RE::BSRenderPass* pass) override;
	bool WantsPreShadowMapsHook() const override { return true; }
	void PreShadowMaps() override;
	void LoadSettings(json& a_input) override;
	void SaveSettings(json& a_output) override;
	void RestoreDefaultSettings() override;
	void OnOpaqueRasterReady(const OpaqueRasterInputs& a_inputs) override;
	void OnOpaqueCompositeReady(const OpaqueRasterInputs& a_inputs) override;
	/** @brief Accepts one complete, same-opaque-pass lighting publication before forward drawing. */
	bool PublishOpaqueLighting(std::shared_ptr<const DX12Native::OpaqueLightingFrame> frame);
	/** @brief Observe CPU-backed opaque rigid geometry from SCM or normal render passes. */
	void ObserveSCMRigidCaster(RE::BSGeometry& geometry, bool requireStability = false);
	/** @brief SCM hands native-owned lights' static casters to RayQuery and retains movers. */
	bool OwnsStaticForLight(RE::BSShadowLight* light, int32_t slot) const;
	// Snapshot LLF/SCM point-light parameters without altering the DX11 light.
	void ObserveRuntimeLight(RE::BSShadowLight* light, uint32_t referenceID, bool flickers,
		const RE::NiPoint3& restOrigin);
	/** @brief Accepts the current LLF publication for Full Native lighting only. */
	void PublishRuntimeDirectLights(uint32_t frame, std::vector<DX12Native::DirectPointLight> lights)
	{
		lighting.PublishRuntimeDirectLights(frame, std::move(lights));
	}
	void OnSceneTransitionReset(bool a_opening) override;
	void OnRuntimeDisabled() override;

	/** @brief Raw full-resolution shadow output, independently fenced for Native Composite. */
	std::shared_ptr<const DX12Native::ShadowMaskFrame> GetShadowMask(uint32_t frame) const { return lighting.GetShadowMask(frame); }
	bool RetainShadowMask(const std::shared_ptr<const DX12Native::ShadowMaskFrame>& frame,
		ID3D12Fence* fence, uint64_t value) { return lighting.RetainShadowMask(frame, fence, value); }

	/** @brief Gets the early raster frame without waiting for GPU completion. */
	std::shared_ptr<const DX12Native::RasterFrame> GetRasterFrame(uint32_t a_frame) const { return raster.Get(a_frame); }
	/** @brief Keeps a borrowed raster frame alive until the consuming GPU work finishes. */
	bool RetainRasterFrame(const std::shared_ptr<const DX12Native::RasterFrame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value)
	{
		return raster.RetainThrough(a_frame, a_fence, a_value);
	}

	/** @brief Submitted full-resolution native material outputs; immutable and fenced. */
	std::shared_ptr<const DX12Native::NativeScreenProbe::MaterialFrame> GetNativeMaterialFrame(uint32_t frame) const { return screenProbe.GetMaterialFrame(frame); }

	/** @brief Whether the coverage visualization is requested for this frame. */
	bool IsCoveragePreviewEnabled() const { return materialCoveragePreview; }
	/** @brief Selects an explicit direct signal preview. */
	uint32_t GetLightingView() const { return directLightingEnabled ? uint32_t(lightingView) : 0u; }
	/** @brief Returns an optional same-frame coverage visualization input. */
	std::shared_ptr<const DX12Native::NativeScreenProbe::MaterialFrame> GetMaterialCoverageFrame(uint32_t frame) const
	{
		return materialCoveragePreview ? screenProbe.GetMaterialFrame(frame) : nullptr;
	}
	/** @brief Returns an optional same-frame direct lighting contribution. */
	std::shared_ptr<const DX12Native::DirectLightingFrame> GetNativeLightingFrame(uint32_t frame) const
	{
		return directLightingEnabled ? lighting.GetDirect(frame) : nullptr;
	}

private:
	/** @brief Gates only native shadow replacement; never disables DX11 baseline shadows. */
	bool CanOffloadShadowsInCurrentCell() const;
	std::optional<GraphicsBackend> backend;
	DX12Native::RasterCapture raster;
	DX12Native::NativeComposite opaqueComposite;
	std::shared_ptr<const DX12Native::RasterFrame> opaqueRaster;
	std::shared_ptr<const DX12Native::OpaqueLightingFrame> opaqueLighting;
	DX12Native::MaterialFrameContract::TemporalIdentity lastTemporal{};
	uint64_t historyRevision = 1;
	DX12Native::RuntimeGeometryCapture geometryCapture;
	DX12Native::AccelerationWorker acceleration;
	DX12Native::NativeLighting lighting;
	DX12Native::NativeScreenProbe screenProbe;
	bool screenProbeEnabled = false;
	bool materialCoveragePreview = false;
	bool directLightingEnabled = false;
	int lightingView = 0;
	bool fullResolutionMaterialProbe = false;
	bool continuousMaterialProbe = false;
	bool shadowMaskEnabled = true;
	bool outdoorShadowOffloading = false;  // Off by default; vanilla raster shadows are unaffected.
	bool offloadStaticShadows = true;
	bool offloadFlickeringStatic = true;
	bool freezeNativeFlickering = false;
	uint32_t densityMode = 7;  // Four-ninths density by default.
	bool nearLightShadowExclusion = true;
	uint32_t nearLightExclusionUnits = 16;
	float shadowReachMultiplier = 1.0f;  // Native only. Vanilla lights stay unchanged.
	float shadowFadeRange = 0.66f;
	float shadowFalloffCurve = 1.0f;
};

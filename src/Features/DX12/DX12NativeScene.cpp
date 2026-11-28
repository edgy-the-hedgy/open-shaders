#include "DX12NativeScene.h"
#include "DX12LightSourceIdentity.h"
#include "Features/NativeLLF/NativeLLF.h"
#include "Features/NativeCubemaps/NativeCubemaps.h"
#include "Features/NativeIBL/NativeIBL.h"
#include "Features/NativePBR/NativePBRFrameProducer.h"
#include "Features/FoliageLighting.h"

#include "../LightLimitFix/ShadowCasterMath.h"
#include "../LightLimitFix/Common.h"
#include "../LightLimitFix/RadiusMath.h"
#include "../Upscaling.h"
#include "../Upscaling/WorldCameraLookup.h"
#include "../ReverseZ.h"
#include "DX12Control.h"
#include "DX12Resources.h"
#include "Globals.h"
#include "Deferred.h"
#include "GpuPass.h"
#include "RE/B/BSLightingShaderProperty.h"
#include "RE/M/MainMenu.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/T/TESObjectLIGH.h"
#include "RE/U/UI.h"
#include "State.h"
#include "ShaderCache.h"
#include "RE/R/RendererShadowState.h"
#include "Utils/D3D.h"
#include "Utils/DevBenchUx.h"
#include "Utils/Game.h"
#include "Utils/PointLightFlags.h"
#include "Utils/TracyZones.h"
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

void DX12NativeScene::DrawSettings()
{
	const auto& control = globals::features::dx12Control;
	if (control.IsFullRTMode() || control.UsesExclusiveMaterials()) {
		const char* qualities[]{ T("menu.dx12.scene.rt_quality", "Quality"), T("menu.dx12.scene.rt_performance", "Performance") };
		int quality = fullRTQuality == DX12Native::kShadowQualityPerformance ? 1 : 0;
		if (ImGui::Combo(control.UsesExclusiveMaterials() ? "Ray query quality" : T("menu.dx12.scene.shadow_quality", "Shadow quality"), &quality, qualities, int(std::size(qualities))))
			fullRTQuality = quality == 0 ? DX12Native::kShadowQualityQuality : DX12Native::kShadowQualityPerformance;
		if (control.UsesExclusiveMaterials()) {
			bool alternate = characterUpdateInterval == 2u;
			if (ImGui::Checkbox("Update characters every second frame", &alternate))
				characterUpdateInterval = alternate ? 2u : 1u;
			return;
		}
		bool outdoor = outdoorFullRT.load(std::memory_order_relaxed);
		if (ImGui::Checkbox(T("menu.dx12.scene.outdoor_shadow_offloading", "Enable outdoor shadow offloading (DX12 only)"), &outdoor))
			outdoorFullRT.store(outdoor, std::memory_order_relaxed);
	} else if (control.IsShadowOffloadMode()) {
		bool outdoor = outdoorShadowOffloading.load(std::memory_order_relaxed);
		if (ImGui::Checkbox(T("menu.dx12.scene.outdoor_shadow_offloading", "Enable outdoor shadow offloading (DX12 only)"), &outdoor))
			outdoorShadowOffloading.store(outdoor, std::memory_order_relaxed);
	}
}

void DX12NativeScene::DrawLightConsolidationSettings()
{
	int unify = int(runtimeUnificationRadius.load(std::memory_order_relaxed));
	int exclude = int(preparedExclusionRadius.load(std::memory_order_relaxed));
	if (ImGui::SliderInt(T("menu.dx12.scene.runtime_unification_radius", "Runtime light unification radius (units)"), &unify, 0, int(DX12Native::kFullShadowRadiusMax), "%d", ImGuiSliderFlags_AlwaysClamp))
		runtimeUnificationRadius.store(DX12Native::SanitizeFullShadowRadius(uint32_t(unify)), std::memory_order_relaxed);
	if (ImGui::SliderInt(T("menu.dx12.scene.prepared_exclusion_radius", "Prepared light exclusion radius (units)"), &exclude, 0, int(DX12Native::kFullShadowRadiusMax), "%d", ImGuiSliderFlags_AlwaysClamp))
		preparedExclusionRadius.store(DX12Native::SanitizeFullShadowRadius(uint32_t(exclude)), std::memory_order_relaxed);
	bool additive = additivePreparedEmission.load(std::memory_order_relaxed);
	if (ImGui::Checkbox(T("menu.dx12.scene.additive_prepared_emission", "Add excluded runtime emission to prepared lights"), &additive))
		additivePreparedEmission.store(additive, std::memory_order_relaxed);
	int interval = int(lightObservationFrames.load(std::memory_order_relaxed));
	if (ImGui::SliderInt(T("menu.dx12.scene.light_observation_interval", "Light observation interval (frames)"), &interval, 1, int(DX12Native::kMaxLightObservationFrames)))
		lightObservationFrames.store(uint32_t(interval), std::memory_order_relaxed);
}

void DX12NativeScene::DrawDevSettings()
{
	const auto& control = globals::features::dx12Control;
	if (control.UsesExclusiveMaterials()) {
		DrawLightConsolidationSettings();
		return;
	}
	if (!control.IsShadowOffloadMode() && !control.IsFullRTMode())
		return;
	bool early = earlyTraceExperiment.load(std::memory_order_relaxed);
	if (ImGui::Checkbox(T("menu.dx12.scene.early_trace", "Experimental depth-prepass trace"), &early))
		earlyTraceExperiment.store(early, std::memory_order_relaxed);
	bool audit = coverageAudit.load(std::memory_order_relaxed);
	if (ImGui::Checkbox(T("menu.dx12.scene.audit_caster_coverage", "Audit exact caster coverage"), &audit))
		coverageAudit.store(audit, std::memory_order_relaxed);
	ImGui::Text(T("menu.dx12.scene.caster_coverage_counts", "Covered: %llu | Unconfirmed: %llu"), static_cast<unsigned long long>(coveredCasterChecks.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(missingCasterChecks.load(std::memory_order_relaxed)));
	bool debugMask = DX12Native::compositeShadowDebugView.load(std::memory_order_relaxed);
	if (ImGui::Checkbox(T("menu.dx12.scene.debug_shadow_mask", "Show ray-traced shadow mask"), &debugMask))
		DX12Native::compositeShadowDebugView.store(debugMask, std::memory_order_relaxed);
	if (control.IsFullRTMode()) {
		bool slices = lightVisibilityExperiment.load(std::memory_order_relaxed);
		if (ImGui::Checkbox(T("menu.dx12.scene.light_visibility", "Experimental per-light visibility (producer only)"), &slices))
			lightVisibilityExperiment.store(slices, std::memory_order_relaxed);
		bool exact = exactShadowLights.load(std::memory_order_relaxed);
		if (ImGui::Checkbox(T("menu.dx12.scene.exact_shadow_lights", "Experimental exact shadow-light origins"), &exact))
			exactShadowLights.store(exact, std::memory_order_relaxed);
		DrawLightConsolidationSettings();
		std::scoped_lock lock(runtimeLightMutex);
		const auto stats = lightWorker.GetStats();
		ImGui::Text("%s", std::format("Light tasks: {} | Publications: {} | Unchanged: {} | Deferred: {} | Busy: {}",
							  stats.tasks, stats.publications, stats.unchanged, stats.deferred, stats.busy)
							  .c_str());
	}
	auto& enabled = control.IsFullRTMode() ? debugRTLights : debugOffloadLights;
	bool value = enabled.load(std::memory_order_relaxed);
	if (ImGui::Checkbox(T("menu.dx12.scene.debug_native_lights", "Preview native lights"), &value))
		enabled.store(value, std::memory_order_relaxed);
}

bool DX12NativeScene::WantsLightPreview(DX12::SCMNativeMode mode) const
{
	if (!globals::state || !globals::state->IsDeveloperMode())
		return false;
	switch (mode) {
	case DX12::SCMNativeMode::ShadowOffload:
		return debugOffloadLights.load(std::memory_order_relaxed);
	case DX12::SCMNativeMode::FullRT:
		return debugRTLights.load(std::memory_order_relaxed);
	default:
		return false;
	}
}

bool DX12NativeScene::IsOverlayVisible() const
{
	return WantsLightPreview(GetFrameNativeMode());
}

void DX12NativeScene::DrawOverlay()
{
	if (!IsOverlayVisible() || globals::state->isMapMenuOpen)
		return;
	const auto mask = shadows.Get(globals::game::graphicsState->GetFrameCount());
	if (!mask || mask->debugLights.empty())
		return;
	const auto* viewport = ImGui::GetMainViewport();
	auto* draw = ImGui::GetForegroundDrawList();
	constexpr float markerRadius = 4.0f;
	constexpr float labelOffset = 6.0f;
	const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
	const ImU32 background = ImGui::GetColorU32(ImGuiCol_PopupBg);
	for (size_t i = 0; i < mask->debugLights.size(); ++i) {
		const auto& light = mask->debugLights[i];
		float2 uv;
		if (!Util::GetWorldFocusCoord(RE::NiPoint3{ light.position.x, light.position.y, light.position.z }, uv))
			continue;
		const ImVec2 point{ viewport->Pos.x + uv.x * viewport->Size.x, viewport->Pos.y + uv.y * viewport->Size.y };
		const char* source = mask->quality == DX12Native::kShadowQualityOffloading ? "M" :
		                                                                             (i < mask->debugPreparedCount ? "P" : "R");
		const auto label = light.unifiedCount > 1 ?
		                       std::format("DXR U x{} {:08X} #{}", light.unifiedCount, light.referenceID, i) :
		                       std::format("DXR {} {:08X} #{}", source, light.referenceID, i);
		const ImVec2 text{ point.x + labelOffset, point.y - ImGui::GetTextLineHeight() * 0.5f };
		const auto size = ImGui::CalcTextSize(label.c_str());
		draw->AddCircleFilled(point, markerRadius, color);
		draw->AddRectFilled(text, ImVec2(text.x + size.x, text.y + size.y), background);
		draw->AddText(text, color, label.c_str());
	}
}

void DX12NativeScene::LoadSettings(json& input)
{
	outdoorShadowOffloading = input.value("OutdoorShadowOffloading", false);
	debugOffloadLights.store(input.value("DebugNativeLights", false), std::memory_order_relaxed);
	const auto profile = input.value("FullRT", json::object());
	outdoorFullRT = profile.value("OutdoorShadowOffloading", false);
	debugRTLights.store(profile.value("DebugNativeLights", false), std::memory_order_relaxed);
	fullRTQuality = DX12Native::SanitizeRTQuality(profile.value("ShadowQualityMode", DX12Native::kShadowQualityQuality));
	characterUpdateInterval = std::clamp(input.value("CharacterUpdateInterval", 1u), 1u, 2u);
	uint32_t unify = profile.value("RuntimeUnificationRadius", DX12Native::kDefaultRuntimeUnificationRadius);
	uint32_t exclude = profile.value("PreparedExclusionRadius", DX12Native::kDefaultPreparedExclusionRadius);
	if (!profile.contains("AdditivePreparedEmission")) {
		if (unify == DX12Native::kLegacyRuntimeUnificationRadius)
			unify = DX12Native::kDefaultRuntimeUnificationRadius;
		if (exclude == DX12Native::kLegacyPreparedExclusionRadius)
			exclude = DX12Native::kDefaultPreparedExclusionRadius;
	}
	runtimeUnificationRadius.store(DX12Native::SanitizeFullShadowRadius(unify), std::memory_order_relaxed);
	preparedExclusionRadius.store(DX12Native::SanitizeFullShadowRadius(exclude), std::memory_order_relaxed);
	additivePreparedEmission.store(profile.value("AdditivePreparedEmission", true), std::memory_order_relaxed);
	lightObservationFrames.store(std::clamp(profile.value("LightObservationFrames", DX12Native::kDefaultLightObservationFrames), 1u, DX12Native::kMaxLightObservationFrames), std::memory_order_relaxed);
	std::scoped_lock lock(runtimeLightMutex);
	ResetFullLightInventory();
}

void DX12NativeScene::SaveSettings(json& output)
{
	output = { { "OutdoorShadowOffloading", outdoorShadowOffloading.load(std::memory_order_relaxed) },
		{ "DebugNativeLights", debugOffloadLights.load(std::memory_order_relaxed) },
		{ "CharacterUpdateInterval", characterUpdateInterval },
		{ "FullRT", { { "OutdoorShadowOffloading", outdoorFullRT.load(std::memory_order_relaxed) }, { "ShadowQualityMode", fullRTQuality },
						{ "DebugNativeLights", debugRTLights.load(std::memory_order_relaxed) },
						{ "RuntimeUnificationRadius", runtimeUnificationRadius.load(std::memory_order_relaxed) },
						{ "PreparedExclusionRadius", preparedExclusionRadius.load(std::memory_order_relaxed) },
						{ "AdditivePreparedEmission", additivePreparedEmission.load(std::memory_order_relaxed) },
						{ "LightObservationFrames", lightObservationFrames.load(std::memory_order_relaxed) } } } };
}

void DX12NativeScene::RestoreDefaultSettings()
{
	outdoorShadowOffloading = false;
	outdoorFullRT = false;
	debugOffloadLights.store(false, std::memory_order_relaxed);
	debugRTLights.store(false, std::memory_order_relaxed);
	fullRTQuality = DX12Native::kShadowQualityQuality;
	characterUpdateInterval = 1u;
	runtimeUnificationRadius.store(DX12Native::kDefaultRuntimeUnificationRadius, std::memory_order_relaxed);
	preparedExclusionRadius.store(DX12Native::kDefaultPreparedExclusionRadius, std::memory_order_relaxed);
	additivePreparedEmission.store(true, std::memory_order_relaxed);
	lightObservationFrames.store(DX12Native::kDefaultLightObservationFrames, std::memory_order_relaxed);
	std::scoped_lock lock(runtimeLightMutex);
	ResetFullLightInventory();
}

bool DX12NativeScene::AllowsNativeSceneWork() const
{
	const auto& control = globals::features::dx12Control;
	return control.IsShadowOffloadMode() || control.IsFullRTMode() || control.IsFullNativeMode();
}

bool DX12NativeScene::AllowsOutdoorContribution() const
{
	if (globals::features::dx12Control.IsFullNativeMode())
		return true;  // Full Native preparation is independent of the shadow-mode outdoor toggles.
	return globals::features::dx12Control.IsFullRTMode() ?
	           outdoorFullRT.load(std::memory_order_relaxed) :
	           outdoorShadowOffloading.load(std::memory_order_relaxed);
}

bool DX12NativeScene::CanOffloadShadowsInCurrentCell() const
{
	if (globals::game::ui && globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME))
		return false;
	const auto* cell = Util::GetPlayerCell();
	return cell && AllowsScenePreparation(cell->IsExteriorCell());
}

DX12::SCMNativeMode DX12NativeScene::GetFrameNativeMode() const
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive() ||
		globals::features::dx12Resources.IsLoading() || !globals::game::graphicsState ||
		frameNativeModeID.load(std::memory_order_acquire) != globals::game::graphicsState->GetFrameCount())
		return DX12::SCMNativeMode::Disabled;
	return frameNativeMode.load(std::memory_order_acquire);
}

bool DX12NativeScene::OwnsStaticForLight(RE::BSShadowLight* light, int32_t slot) const
{
	// SCM keeps raster dynamic casters unconditionally. Static casters may be
	// omitted only when this exact runtime light was matched to a native emitter
	// in the current frame; a nearby prepared light is not sufficient proof.
	if (!light || slot < 0 ||
		light->GetIsFrustumOrDirectionalLight() || !globals::game::graphicsState)
		return false;
	const auto scene = acceleration.GetScene();
	const auto lights = shadowLights.load(std::memory_order_acquire);
	const auto* ni = light->light.get();
	if (!scene || !scene->acceleration || !lights || !ni ||
		scene->scope != shadowLightScope.load(std::memory_order_acquire))
		return false;

	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	std::scoped_lock lock(runtimeLightMutex);
	if (runtimeLightFrame != frame)
		return false;

	const auto found = std::ranges::find_if(runtimeLightSamples, [ni](const RuntimeLightSample& sample) {
		return sample.source == ni;
	});
	if (found == runtimeLightSamples.end())
		return false;

	// The shader admits only the published prepared lights. Never suppress a
	// raster static caster for a light not actually selected for DX12 tracing.
	const size_t limit = std::min(lights->size(), size_t{ DX12Native::kMaxShadowLights });
	for (size_t i = 0; i < limit; ++i) {
		const auto& candidate = (*lights)[i];
		if (candidate.referenceID != 0 && candidate.referenceID == found->referenceID) {
			ownershipGranted.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
	}
	return false;
}

void DX12NativeScene::ObserveRuntimeLight(RE::BSShadowLight* light, uint32_t referenceID,
	bool /*flickers*/, const RE::NiPoint3& restOrigin)
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive() ||
		!light || light->GetIsFrustumOrDirectionalLight() || !globals::game::graphicsState)
		return;
	auto* ni = light->light.get();
	const auto lights = shadowLights.load(std::memory_order_acquire);
	if (!ni || !lights)
		return;
	const auto& rt = ni->GetLightRuntimeData();
	const auto& position = ni->world.translate;
	if (!std::isfinite(rt.radius.x) || rt.radius.x <= 0.0f ||
		!std::isfinite(rt.fade) || !std::isfinite(rt.diffuse.red) ||
		!std::isfinite(rt.diffuse.green) || !std::isfinite(rt.diffuse.blue) ||
		!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
		return;

	const DX12::PreparedLight* matched = nullptr;
	if (referenceID != 0) {
		const auto it = std::ranges::find_if(*lights, [referenceID](const auto& candidate) {
			return candidate.referenceID == referenceID;
		});
		if (it == lights->end())
			return;  // Never let a different nearby light inherit this source's animation.
		matched = &*it;
	} else {
		float bestDistanceSquared = std::numeric_limits<float>::infinity();
		for (const auto& candidate : *lights) {
			const float dx = candidate.position.x - restOrigin.x;
			const float dy = candidate.position.y - restOrigin.y;
			const float dz = candidate.position.z - restOrigin.z;
			const float errorSquared = dx * dx + dy * dy + dz * dz;
			const float tolerance = std::clamp(candidate.radius * 0.01f, 4.0f, 32.0f);
			if (std::isfinite(errorSquared) && errorSquared < bestDistanceSquared &&
				errorSquared <= tolerance * tolerance) {
				bestDistanceSquared = errorSquared;
				matched = &candidate;
			}
		}
	}
	if (!matched)
		return;
	DX12::PreparedLight live = *matched;
	live.position = float3{ position.x, position.y, position.z };
	live.radius = rt.radius.x;
	live.color = float3{ rt.diffuse.red, rt.diffuse.green, rt.diffuse.blue };
	live.fade = rt.fade;
	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	std::scoped_lock lock(runtimeLightMutex);
	if (runtimeLightFrame != frame) {
		runtimeLightFrame = frame;
		runtimeLightSamples.clear();
	}
	// A prepared emitter must not accidentally account for two different SCM
	// lights that happen to occupy the same position.
	const auto it = std::ranges::find_if(runtimeLightSamples, [ni](const auto& sample) {
		return sample.source == ni;
	});
	if (it != runtimeLightSamples.end()) {
		it->referenceID = matched->referenceID;
		it->value = live;
	} else if (runtimeLightSamples.size() < DX12Native::kMaxShadowLights &&
			   !std::ranges::any_of(runtimeLightSamples, [&](const auto& sample) {
				   return sample.referenceID == matched->referenceID;
			   })) {
		runtimeLightSamples.push_back({ ni, matched->referenceID, live });
	}
}

bool DX12NativeScene::WantsRenderPassHook() const
{
	return globals::features::dx12Control.IsActive() && !globals::game::isVR;
}

bool DX12NativeScene::WantsPreShadowMapsHook() const { return WantsRenderPassHook(); }
bool DX12NativeScene::WantsOpaqueRasterReadyHook() const { return WantsRenderPassHook(); }
bool DX12NativeScene::WantsDepthPrepassReadyHook() const { return WantsRenderPassHook(); }

void DX12NativeScene::OnDepthPrepassReady()
{
	if (globals::features::dx12Control.WantsMaterialPreparation()) {
		DispatchFullNativeMaterial();
		return;
	}
	if (!earlyTraceExperiment.load(std::memory_order_relaxed) || !globals::state || !globals::state->IsDeveloperMode() ||
		GetFrameNativeMode() == DX12::SCMNativeMode::Disabled)
		return;
	CS_GPU_PASS("DX12NativeScene::DepthPrepassReady");
	// The same combined mask is produced. The later hook remains the fallback.
	OnOpaqueRasterReady();
}

void DX12NativeScene::PrepareLoadingScene(std::shared_ptr<const DX12::Grid> grid)
{
	ApplyPendingLoadingReset();
	if (!grid || !globals::features::dx12Resources.IsLoading() || !globals::features::dx12Control.IsActive() || !AllowsNativeSceneWork() ||
		!globals::d3d::device || !globals::d3d::context ||
		!std::ranges::any_of(grid->resident, [](const auto& tile) { return tile && !tile->shapes.empty(); }))
		return;
	nativeSessionStarted = true;
	if (acceleration.Start(globals::d3d::device, globals::d3d::context))
		acceleration.RequestStaged(std::move(grid));
}

void DX12NativeScene::PreShadowMaps()
{
	if (backend && !GraphicsBackend::CheckNativeDeviceHealth(backend->d3d12Device.get(), backend->d3d11Device.get())) return;
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive()) {
		OnRuntimeDisabled();
		return;
	}
	ApplyPendingLoadingReset();
	if (globals::features::dx12Resources.IsLoading() || !globals::state || !globals::game::graphicsState ||
		!globals::state->startupMenuInitializationComplete.load(std::memory_order_acquire) ||
		(globals::game::ui && globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME))) {
		frameNativeModeID.store(UINT32_MAX, std::memory_order_release);
		return;
	}
	if (!CanOffloadShadowsInCurrentCell()) {
		if (nativeSessionStarted)
			OnRuntimeDisabled();
		return;
	}
	nativeSessionStarted = true;
	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	if (frameNativeModeID.load(std::memory_order_acquire) != frame) {
		const auto& control = globals::features::dx12Control;
		const auto mode = DX12::SelectSCMNativeMode(control.IsActive(), globals::game::isVR,
			control.IsShadowOffloadMode(), control.IsFullRTMode(), CanOffloadShadowsInCurrentCell());
		const auto previous = frameNativeMode.exchange(mode, std::memory_order_acq_rel);
		const bool exact = mode == DX12::SCMNativeMode::FullRT && globals::state->IsDeveloperMode() && exactShadowLights.load(std::memory_order_relaxed);
		const bool previousExact = frameExactShadowLights.exchange(exact, std::memory_order_acq_rel);
		if (previous != mode || previousExact != exact) {
			std::scoped_lock lock(runtimeLightMutex);
			ResetFullLightInventory();
			runtimeLightFrame = UINT32_MAX;
			runtimeLightSamples.clear();
		}
		frameNativeModeID.store(frame, std::memory_order_release);
	}
	auto grid = globals::features::dx12Resources.GetGrid();
	if (!grid || grid->requested.empty() || !grid->requested.front().scope) {
		shadowLightScope.store(0, std::memory_order_release);
		shadowLights.store(nullptr, std::memory_order_release);
		std::scoped_lock lock(runtimeLightMutex);
		ResetFullLightInventory();
		return;
	}
	if (globals::features::dx12Control.IsFullNativeMode()) {
		// The static scene remains independent of moving character casters.
		if (!backend) {
			std::string reason;
			backend = GraphicsBackend::Create(globals::d3d::device, globals::d3d::context, reason);
		}
		if (backend) {
			if (!characterCasters) characterCasters = std::make_unique<DX12Native::NativeCharacterCasters>();
			// Publish at the opaque boundary after rigid mover observations.
		}
		if (acceleration.Start(globals::d3d::device, globals::d3d::context))
			acceleration.Request(std::move(grid));
		return;
	}
	shadowLightScope.store(grid->requested.front().scope, std::memory_order_release);
	// SCM reads a single per-frame publication without re-enumerating tiles
	// for every shadow light; the same ordered sources feed RayQuery.
	if (GetFrameNativeMode() != DX12::SCMNativeMode::FullRT)
		shadowLights.store(std::make_shared<const std::vector<DX12::PreparedLight>>(
							   globals::features::dx12Resources.GetPreparedLights()),
			std::memory_order_release);
	// Only a revision-coalesced mailbox handoff on this thread. All BLAS/TLAS
	// allocations and command recording run as bounded shared-pool tasks.
	if (acceleration.Start(globals::d3d::device, globals::d3d::context))
		acceleration.Request(std::move(grid));
}

void DX12NativeScene::ResetFullLightInventory()
{
	lightWorker.Reset();
	fullRuntimeLights.clear();
	fullRequestedGrid.reset();
	fullRuntimeSnapshot.reset();
	fullRequestedRuntime.reset();
	fullRuntimeScope = 0;
	fullRuntimeCell = 0;
	fullLightVisit = 0;
	fullObserved = false;
}

bool DX12NativeScene::ObserveFullRuntimeLights()
{
	ZoneScopedN("DX12::Lights::Observe");
	const auto node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
	if (!node) {
		if (!fullRuntimeSnapshot)
			fullRuntimeSnapshot = std::make_shared<const std::vector<DX12Native::NativeRuntimeEmitter>>();
		return false;
	}
	bool dirty = !fullRuntimeSnapshot;
	const uint64_t visit = ++fullLightVisit;
	const bool preserveExact = frameExactShadowLights.load(std::memory_order_relaxed);
	const auto collect = [&](RE::BSLight* light, bool shadowLight) {
		if (!light || !light->pointLight || !light->light)
			return;
		auto* ni = light->light.get();
		auto [it, inserted] = fullRuntimeLights.try_emplace(ni);
		auto& cached = it->second;
		if (cached.seen == visit)
			return;
		cached.seen = visit;
		if (inserted) {
			cached.source = light->light;
			const auto flags = PointLightFlags::GetVanillaPointLightFlags(light, ni);
			cached.supported = !(flags & PointLightFlags::ToMask(PointLightFlags::Flags::Spot));
			cached.value.runtimeIdentity = reinterpret_cast<std::uintptr_t>(ni);
			DX12Native::ReadFullLightOwner(ni, cached.value.referenceID, cached.placedReference);
		}
		if (!cached.supported)
			return;
		const bool visible = !ni->GetFlags().any(RE::NiAVObject::Flag::kHidden);
		if (visible && !cached.sampled) {
			const auto& rt = ni->GetLightRuntimeData();
			const auto& position = ni->world.translate;
			cached.sampled = std::isfinite(rt.radius.x) && rt.radius.x > 0.0f &&
			                 std::isfinite(rt.fade) && rt.fade > 0.0f &&
			                 std::isfinite(rt.diffuse.red) && std::isfinite(rt.diffuse.green) && std::isfinite(rt.diffuse.blue) &&
			                 std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
			if (cached.sampled) {
				auto& value = cached.value;
				value.position = float3{ position.x, position.y, position.z };
				value.radius = rt.radius.x;
				value.color = float3{ rt.diffuse.red, rt.diffuse.green, rt.diffuse.blue };
				value.fade = rt.fade;
				DX12Native::ReadFullLightRestPosition(ni, cached.placedReference, value.position);
			}
		}
		const bool active = visible && cached.sampled;
		dirty |= cached.active != active || cached.shadowLight != shadowLight;
		cached.active = active;
		cached.shadowLight = shadowLight;
	};
	if (node) {
		const auto& scene = node->GetRuntimeData();
		if (preserveExact)
			for (const auto& light : scene.activeShadowLights)
				collect(light.get(), true);
		for (const auto& light : scene.activeLights)
			collect(light.get(), false);
		if (!preserveExact)
			for (const auto& light : scene.activeShadowLights)
				collect(light.get(), false);
	}
	std::erase_if(fullRuntimeLights, [&](const auto& entry) {
		if (entry.second.seen == visit)
			return false;
		dirty |= entry.second.active;
		return true;
	});
	if (dirty) {
		auto snapshot = std::make_shared<std::vector<DX12Native::NativeRuntimeEmitter>>();
		snapshot->reserve(fullRuntimeLights.size());
		for (const auto& [identity, cached] : fullRuntimeLights)
			if (cached.active)
				snapshot->push_back({ cached.value, cached.placedReference, cached.shadowLight });
		fullRuntimeSnapshot = std::move(snapshot);
	}
	return true;
}

std::shared_ptr<const DX12Native::NativeLightPublication> DX12NativeScene::CollectFullRTLights(uint32_t frame)
{
	std::scoped_lock lock(runtimeLightMutex);
	const auto grid = globals::features::dx12Resources.GetGrid();
	const auto* cell = Util::GetPlayerCell();
	const uint32_t cellID = cell ? cell->GetFormID() : 0;
	const uint32_t scope = shadowLightScope.load(std::memory_order_acquire);
	if (!grid || !scope || !cellID || grid->requested.front().scope != scope)
		return {};
	if (fullRuntimeScope != scope || fullRuntimeCell != cellID) {
		ResetFullLightInventory();
		fullRuntimeScope = scope;
		fullRuntimeCell = cellID;
	}
	if (!fullObserved || DX12Native::NativeLightObservationDue(frame, fullLastObservation,
							 lightObservationFrames.load(std::memory_order_relaxed))) {
		fullObserved = ObserveFullRuntimeLights();
		fullLastObservation = frame;
	}
	if (!fullRuntimeSnapshot)
		return lightWorker.GetReady();
	const DX12Native::NativeLightSettings settings{
		runtimeUnificationRadius.load(std::memory_order_relaxed),
		preparedExclusionRadius.load(std::memory_order_relaxed),
		additivePreparedEmission.load(std::memory_order_relaxed),
		frameExactShadowLights.load(std::memory_order_relaxed)
	};
	const bool gridChanged = !fullRequestedGrid ||
	                         (fullRequestedGrid != grid && (fullRequestedGrid->resident != grid->resident ||
															   fullRequestedGrid->predictiveResident != grid->predictiveResident));
	const bool dirty = gridChanged || fullRequestedRuntime != fullRuntimeSnapshot ||
	                   fullRequestedSettings != settings || (lightWorker.NeedsRetry() && DX12Native::NativeLightObservationDue(frame, fullLastRequestFrame, lightObservationFrames.load(std::memory_order_relaxed)));
	// Promote the completed replacement before admitting another GPU upload.
	auto ready = lightWorker.GetReady();
	if (dirty && backend && lightWorker.Request(*backend, grid, fullRuntimeSnapshot, settings, cellID)) {
		fullRequestedGrid = grid;
		fullRequestedRuntime = fullRuntimeSnapshot;
		fullRequestedSettings = settings;
		fullLastRequestFrame = frame;
	} else if (!gridChanged) {
		fullRequestedGrid = grid;
	}
	return ready;
}

// Only opaque identities and frame-local origin values are submitted to native tracing.
void DX12NativeScene::ObserveScheduledShadowOrigin(const RE::NiLight* light, const RE::NiPoint3& origin)
{
	if (!frameExactShadowLights.load(std::memory_order_relaxed) || GetFrameNativeMode() != DX12::SCMNativeMode::FullRT ||
		!light || !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z))
		return;
	const auto frame = globals::game::graphicsState->GetFrameCount();
	std::scoped_lock lock(runtimeLightMutex);
	if (scheduledOriginsFrame != frame) {
		scheduledOrigins.clear();
		scheduledOriginsFrame = frame;
	}
	if (scheduledOrigins.size() < DX12::kMaxPreparedLights)
		scheduledOrigins.try_emplace(reinterpret_cast<uintptr_t>(light), float3{ origin.x, origin.y, origin.z });
}

std::shared_ptr<const DX12Native::NativeScreenProbe::MaterialFrame> DX12NativeScene::GetFullNativeMaterial(uint32_t frame) const
{
	return globals::features::nativePBR.Get(frame);
}

std::shared_ptr<const DX12Native::IndirectLightingFrame> DX12NativeScene::GetFullNativeIndirect(uint32_t frame) const
{
	return globals::features::nativeIBL.loaded ? globals::features::nativeIBL.Get(frame) : nullptr;
}

std::shared_ptr<const DX12Native::NativeWorldFrame> DX12NativeScene::GetWorldFrame(uint32_t engineFrame, uint32_t pluginFrame) const
{
	const auto publication = worldFrame.load(std::memory_order_acquire);
	return publication && publication->Matches(engineFrame, pluginFrame) ? publication : nullptr;
}

void DX12NativeScene::ObserveWorldCamera(const globals::FrameBuffer& camera)
{
	using namespace DX12Native;
	const auto* state = globals::state;
	if (!globals::features::dx12Control.UsesExclusiveMaterials() || globals::game::isVR ||
		!state || !globals::game::graphicsState || !state->inWorld || state->nativeFirstPersonDepth ||
		(state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return;
	const auto engine = globals::game::graphicsState->GetFrameCount();
	const auto plugin = state->frameCount;
	if (GetWorldFrame(engine, plugin)) return;
	const auto world = UpscalingCamera::GetWorldCameraFrame();
	const auto* identity = RE::Main::WorldRootCamera();
	// The camera table identifies the engine's world root independently of the
	// most recently uploaded buffer. Check the actual upload against it.
	const auto sameMatrix = [](const Matrix& a, const Matrix& b) {
		for (uint32_t row = 0; row < 4; ++row)
			for (uint32_t col = 0; col < 4; ++col) {
				const float x = a.m[row][col], y = b.m[row][col];
				if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x-y) > 0.00001f * std::max(1.0f, std::abs(y)))
					return false;
			}
		return true;
	};
	if (!world || !identity || !std::isfinite(camera.CameraPosAdjust.x) || !std::isfinite(camera.CameraPosAdjust.y) ||
		!std::isfinite(camera.CameraPosAdjust.z) || !std::isfinite(world->origin.x) || !std::isfinite(world->origin.y) ||
		!std::isfinite(world->origin.z) || !sameMatrix(camera.CameraViewProjUnjittered.Transpose(), world->viewProjection) ||
		!sameMatrix(camera.CameraPreviousViewProjUnjittered.Transpose(), world->previousViewProjection) ||
		std::abs(camera.CameraPosAdjust.x-world->origin.x) > 0.001f ||
		std::abs(camera.CameraPosAdjust.y-world->origin.y) > 0.001f ||
		std::abs(camera.CameraPosAdjust.z-world->origin.z) > 0.001f) {
		++rejectedWorldCameraUploads;
		return;
	}
	const auto& upscaling = globals::features::upscaling;
	const auto size = upscaling.GetNativeRenderSize(plugin, {});
	if (!std::isfinite(size.x) || !std::isfinite(size.y) || size.x < 1 || size.y < 1 ||
		!std::isfinite(state->screenSize.x) || !std::isfinite(state->screenSize.y) ||
		state->screenSize.x < 1 || state->screenSize.y < 1 || !globals::game::cameraNear || !globals::game::cameraFar) return;
	const auto jitter = upscaling.GetNativeRasterJitter(plugin, uint32_t(size.x), uint32_t(size.y));
	if (!jitter.valid) return;
	auto frame = std::make_shared<NativeWorldFrame>();
	frame->camera = camera;
	frame->engineFrame = engine; frame->pluginFrame = plugin;
	frame->cameraIdentity = reinterpret_cast<uintptr_t>(identity);
	frame->width = uint32_t(size.x); frame->height = uint32_t(size.y);
	frame->displayWidth = uint32_t(state->screenSize.x); frame->displayHeight = uint32_t(state->screenSize.y);
	frame->jitter = jitter;
	frame->cameraNear = *globals::game::cameraNear; frame->cameraFar = *globals::game::cameraFar;
	frame->cameraFov = Util::GetVerticalFOVRad();
	frame->reverseZ = globals::features::reverseZ.IsActive();
	if (!std::isfinite(frame->cameraNear) || !std::isfinite(frame->cameraFar) || !std::isfinite(frame->cameraFov) ||
		frame->cameraNear <= 0 || frame->cameraFar <= frame->cameraNear || frame->cameraFov <= 0) return;
	const auto previous = worldFrame.load(std::memory_order_acquire);
	const bool consecutive = previous && previous->engineFrame + 1u == engine && previous->pluginFrame + 1u == plugin &&
		previous->cameraIdentity == frame->cameraIdentity && previous->width == frame->width && previous->height == frame->height &&
		previous->displayWidth == frame->displayWidth && previous->displayHeight == frame->displayHeight &&
		previous->reverseZ == frame->reverseZ && previous->cameraNear == frame->cameraNear &&
		previous->cameraFar == frame->cameraFar && std::abs(previous->cameraFov - frame->cameraFov) < 0.001f;
	if (consecutive) {
		frame->previousJitter = previous->jitter;
		frame->previousDynamicResolution1 = previous->camera.DynamicResolutionParams1;
		frame->previousDynamicResolution2 = previous->camera.DynamicResolutionParams2;
	}
	frame->resetHistory = !consecutive || state->ShouldResetHistory();
	worldFrame.store(std::move(frame), std::memory_order_release);
}

void DX12NativeScene::DispatchFullNativeMaterial(bool opaqueReady)
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.WantsMaterialPreparation() ||
		!globals::game::graphicsState || !globals::state || globals::state->isMapMenuOpen ||
		globals::features::dx12Resources.IsLoading() || !nativeSessionStarted || globals::state->nativeFirstPersonDepth ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return;
	CS_GPU_PASS("DX12NativeScene::FullNativeMaterial");
	const auto frame = globals::game::graphicsState->GetFrameCount();
	globals::features::upscaling.nativeComposite.PollFullNative(frame);
	globals::features::nativeIBL.Poll(frame);
	if (fullNativeDirect)
		fullNativeDirect->Poll(frame);
	globals::features::nativePBR.Poll(frame);
	const auto scene = acceleration.GetScene();
	const auto grid = globals::features::dx12Resources.GetGrid();
	if (!scene || !grid || grid->requested.empty() || scene->scope != grid->requested.front().scope ||
		scene->geometryRevision != grid->geometryRevision || !scene->materialEvaluation || !scene->shadingGeometry)
		return;
	if (!backend) {
		std::string reason;
		backend = GraphicsBackend::Create(globals::d3d::device, globals::d3d::context, reason);
		if (!backend)
			return;
	}
	// Evaluate after the opaque boundary so the raw FXC publication and final
	// depth belong to the same frame. The early hook only establishes readiness.
	if (!opaqueReady) return;
	const auto world = GetWorldFrame(frame, globals::state->frameCount);
	if (!world) return;
	const float2 viewport{ float(world->width), float(world->height) };
	if (!std::isfinite(viewport.x) || !std::isfinite(viewport.y) || viewport.x < 1.0f || viewport.y < 1.0f)
		return;
	if (globals::features::nativePBR.WantsFrame(frame)) {
		if (!raster.Get(frame)) {
			auto* renderer = globals::game::renderer;
			auto* motion = renderer ? Util::AsReal(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].texture) : nullptr;
			raster.Capture(*backend, Util::GetCurrentSceneDepthSRV(false), &world->camera, frame, motion, world->jitter);
		}
		if (!GraphicsBackend::CheckNativeDeviceHealth(backend->d3d12Device.get(), backend->d3d11Device.get())) return;
		globals::features::nativePBR.Dispatch(*backend, raster.Get(frame), scene, frame, uint32_t(viewport.x), uint32_t(viewport.y), opaqueReady, FullNativeFrameQuality(frame), rawInputCapture.Get({frame, scene->scope, scene->geometryRevision, scene->materialEvaluation->revision}));
	}
	const auto material = globals::features::nativePBR.Get(frame);
	if (!material || material->scene != scene || !scene->inputs || !scene->inputs->residency)
		return;
	if (fullNativeDirect && fullNativeDirect->Get(frame))
		return;
	const auto lights = globals::features::nativeLLF.CollectDirectInputs(*scene, frame, opaqueReady);
	if (!lights)
		return;
	if (!fullNativeDirect)
		fullNativeDirect = std::make_unique<DX12Native::DirectLightingTrace>();
	// Selected direct coverage remains partial: sun and particle visibility are
	// limited to the resident TLAS; spots require a valid live engine projection.
	fullNativeDirect->Dispatch(*backend, material, lights->points, lights->available,
		lights->directional, uint32_t(viewport.x), uint32_t(viewport.y),
		globals::features::foliageLighting.loaded && globals::features::foliageLighting.GetCommonBufferData().EnableFoliageScattering != 0,
		characterCasters ? characterCasters->Get(frame, material->scope) : nullptr, FullNativeFrameQuality(frame));
}

void DX12NativeScene::ResetFullNativeEnvironment()
{
	if (!globals::features::dx12Control.UsesExclusiveMaterials())
		return;
	if (globals::features::nativePBR.HasEvaluator())
		globals::features::upscaling.nativeComposite.ResetFullNative();
	worldFrame.store(nullptr, std::memory_order_release);
	globals::features::nativeIBL.InvalidateGeneration();
	globals::features::nativeCubemaps.InvalidateGeneration();
}

void DX12NativeScene::DispatchFullNativeIndirect()
{
	if (backend && !GraphicsBackend::CheckNativeDeviceHealth(backend->d3d12Device.get(), backend->d3d11Device.get())) return;
	using namespace DX12Native;
	using namespace DX12Native::MaterialFrameContract;
	if (!loaded || !globals::features::dx12Control.WantsMaterialPreparation() ||
		!globals::state || !globals::game::graphicsState || globals::state->isMapMenuOpen ||
		globals::features::dx12Resources.IsLoading() || !nativeSessionStarted || globals::state->nativeFirstPersonDepth ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return;
	const auto grid = globals::features::dx12Resources.GetGrid();
	if (!grid || grid->requested.empty() || !grid->requested.front().scope)
		return;
	const auto frame = globals::game::graphicsState->GetFrameCount();
	if (!backend)
		return;
	const auto world = GetWorldFrame(frame, globals::state->frameCount);
	const auto material = GetFullNativeMaterial(frame);
	if (!world || !material || material->scope != grid->requested.front().scope)
		return;
	if (!globals::features::dx12Control.UsesLegacyReflectionCapture() &&
		globals::features::nativeCubemaps.loaded && globals::features::nativeIBL.loaded) {
		bool environmentChanged = false;
		const auto stamp = globals::features::nativeCubemaps.Prepare(grid->requested.front().scope, frame, environmentChanged);
		if (environmentChanged)
			globals::features::nativeIBL.InvalidateGeneration();
		globals::features::nativeIBL.Dispatch(*backend, material, globals::features::nativeCubemaps.Get(), stamp,
			characterCasters ? characterCasters->Get(frame, material->scope) : nullptr, FullNativeFrameQuality(frame),
			globals::features::dx12Control.NativeSSREnabled() ? GetFullNativeDirect(frame) : nullptr);
	}
	// Keep direct/material publications available; incomplete native lighting
	// must not overwrite the legacy environment contributions.
	globals::features::upscaling.nativeComposite.ReconstructFullNative(*backend, material, GetFullNativeDirect(frame), GetFullNativeIndirect(frame),
		globals::state->frameCount, world->resetHistory || globals::state->ShouldResetHistory());
}

void DX12NativeScene::CaptureFullNativeEnvironment(ID3D11ShaderResourceView* sceneColor, uint32_t frame)
{
	if (globals::features::dx12Control.UsesLegacyReflectionCapture() || !globals::features::nativeCubemaps.loaded) return;
	if (backend && !GraphicsBackend::CheckNativeDeviceHealth(backend->d3d12Device.get(), backend->d3d11Device.get())) return;
	if (!loaded || !globals::features::dx12Control.WantsMaterialPreparation() || !nativeSessionStarted ||
		!globals::state || globals::state->isMapMenuOpen || globals::features::dx12Resources.IsLoading() || globals::state->nativeFirstPersonDepth ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return;
	if (!backend) {
		std::string reason;
		backend = GraphicsBackend::Create(globals::d3d::device, globals::d3d::context, reason);
		if (!backend)
			return;
	}
	globals::features::nativeCubemaps.Capture(*backend, sceneColor, frame,
		globals::features::upscaling.nativeComposite.GetFullNativeReconstruction(frame));
}

void DX12NativeScene::OnOpaqueRasterReady()
{
	if (backend && !GraphicsBackend::CheckNativeDeviceHealth(backend->d3d12Device.get(), backend->d3d11Device.get())) return;
	if (globals::features::dx12Control.WantsMaterialPreparation()) {
		if (!globals::state || globals::state->nativeFirstPersonDepth ||
			(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
			return;
		FinishFullNativeRasterDraws();
		if (backend && !GraphicsBackend::CheckNativeDeviceHealth(backend->d3d12Device.get(), backend->d3d11Device.get())) return;
		const auto grid = globals::features::dx12Resources.GetGrid();
		if (characterCasters && backend && globals::game::graphicsState && globals::state &&
			!globals::state->isMapMenuOpen && !globals::features::dx12Resources.IsLoading() &&
			!(globals::state->permutationData.ExtraShaderDescriptor &
				static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)) &&
			grid && !grid->requested.empty())
			characterCasters->Publish(*backend, globals::game::graphicsState->GetFrameCount(), grid->requested.front().scope, characterUpdateInterval);
		DispatchFullNativeMaterial(true);
		DispatchFullNativeIndirect();
		return;
	}
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive())
		return;
	if (!globals::game::graphicsState || !globals::state || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor &
			static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return;
	// Only capture the depth and camera used by the independent shadow layer.
	const auto mode = GetFrameNativeMode();
	const bool fullRT = mode == DX12::SCMNativeMode::FullRT;
	if (mode != DX12::SCMNativeMode::ShadowOffload && !fullRT)
		return;

	CS_GPU_PASS("DX12NativeScene::OpaqueRasterReady");
	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	globals::features::upscaling.nativeComposite.RetireCompletedInputs();
	shadows.BeginFrame(frame);
	if (shadows.Get(frame))
		return;
	shadows.ReportSkipped(DX12Native::ShadowMaskTrace::Outcome::None);

	std::shared_ptr<const DX12Native::NativeLightPublication> fullInventory;
	if (fullRT) {
		if (!backend) {
			std::string reason;
			backend = GraphicsBackend::Create(globals::d3d::device, globals::d3d::context, reason);
			if (!backend)
				return;
		}
		fullInventory = CollectFullRTLights(frame);
	}
	const auto built = acceleration.GetScene();
	const auto lights = shadowLights.load(std::memory_order_acquire);
	if (!built || !built->acceleration || (fullRT ? !fullInventory : !lights) ||
		built->scope != shadowLightScope.load(std::memory_order_acquire))
		return shadows.ReportSkipped(DX12Native::ShadowMaskTrace::Outcome::NoScene);

	// Admit only matched runtime emitters that the SCM policy can account for.
	// Iterate the immutable prepared selection to preserve its deterministic order.
	std::vector<DX12::PreparedLight> liveLights;
	if (!fullRT) {
		std::scoped_lock lock(runtimeLightMutex);
		if (runtimeLightFrame != frame || runtimeLightSamples.empty())
			return shadows.ReportSkipped(DX12Native::ShadowMaskTrace::Outcome::NoLights);
		std::unordered_map<uint32_t, const DX12::PreparedLight*> liveByReference;
		liveByReference.reserve(runtimeLightSamples.size());
		for (const auto& sample : runtimeLightSamples)
			if (sample.referenceID)
				liveByReference.try_emplace(sample.referenceID, &sample.value);
		const size_t limit = std::min(lights->size(), size_t{ DX12Native::kMaxShadowLights });
		liveLights.reserve(std::min(limit, liveByReference.size()));
		for (size_t i = 0; i < limit; ++i) {
			const auto it = liveByReference.find((*lights)[i].referenceID);
			if (it != liveByReference.end())
				liveLights.push_back(*it->second);
		}
	}
	std::span<const DX12::PreparedLight> submittedLights = fullRT ?
	                                                           std::span<const DX12::PreparedLight>(fullInventory->lights) :
	                                                           std::span<const DX12::PreparedLight>(liveLights);
	uint32_t moving = 0;
	if (fullRT && frameExactShadowLights.load(std::memory_order_relaxed)) {
		std::scoped_lock lock(runtimeLightMutex);
		if (scheduledOriginsFrame == frame) {
			constexpr float kMinOriginShift = 0.25f;
			for (size_t i = 0; i < submittedLights.size(); ++i) {
				const auto it = scheduledOrigins.find(submittedLights[i].runtimeIdentity);
				if (it == scheduledOrigins.end())
					continue;
				const auto& p = submittedLights[i].position;
				const auto& q = it->second;
				const float dx = q.x - p.x, dy = q.y - p.y, dz = q.z - p.z;
				if (dx * dx + dy * dy + dz * dz < kMinOriginShift * kMinOriginShift)
					continue;
				if (!moving)
					lightsAtShadowOrigins.assign(submittedLights.begin(), submittedLights.end());
				lightsAtShadowOrigins[i].position = q;
				++moving;
			}
		}
		if (moving)
			submittedLights = lightsAtShadowOrigins;
	}
	const auto previousMoving = movingOriginCount.exchange(moving, std::memory_order_relaxed);
	// Until per-light history exists, reject the combined history on moving-origin frames.
	if (moving || previousMoving)
		globals::features::upscaling.nativeComposite.ResetShadowHistory();
	if (submittedLights.empty())
		return shadows.ReportSkipped(DX12Native::ShadowMaskTrace::Outcome::NoLights);

	if (!backend) {
		std::string reason;
		backend = GraphicsBackend::Create(globals::d3d::device, globals::d3d::context, reason);
		if (!backend)
			return;
	}

	const auto viewport = Util::ConvertToDynamic(globals::state->screenSize);
	const uint32_t viewportWidth = std::isfinite(viewport.x) && viewport.x > 0.0f ? uint32_t(viewport.x) : 0u;
	const uint32_t viewportHeight = std::isfinite(viewport.y) && viewport.y > 0.0f ? uint32_t(viewport.y) : 0u;
	if (!viewportWidth || !viewportHeight)
		return shadows.ReportSkipped(DX12Native::ShadowMaskTrace::Outcome::ViewportMismatch);
	raster.Capture(*backend, Util::GetCurrentSceneDepthSRV(false),
		&globals::game::frameBufferCached.nonVR, frame);
	const DX12Native::ShadowTraceControls controls{
		.viewportWidth = viewportWidth,
		.viewportHeight = viewportHeight,
		.quality = fullRT ? fullRTQuality : DX12Native::kShadowQualityOffloading,
		.captureDebugLights = WantsLightPreview(mode),
		.debugPreparedCount = fullRT ? fullInventory->preparedCount : 0u,
		.nativeLights = fullInventory,
		.uploadLights = moving != 0,
		.captureLightVisibility = fullRT && globals::state->IsDeveloperMode() && lightVisibilityExperiment.load(std::memory_order_relaxed)
	};
	shadows.Dispatch(*backend, raster.Get(frame), built, submittedLights, frame, controls);
}

void DX12NativeScene::BeginFullNativeRasterInputs()
{
	if (!globals::features::dx12Control.UsesExclusiveMaterials() || !globals::game::graphicsState) return;
	ZoneScopedN("DX12::RasterInputs::Begin");
	auto& draws = fullNativeRasterDraws;
	const auto oldInputs = draws.scene ? draws.scene->inputs : nullptr;
	// Preserve capacity while resetting frame-local state and COM references.
	FullNativeRasterDrawState next;
	next.seen = std::move(draws.seen);
	next.seenEpoch = draws.seenEpoch + 1u;
	if (!next.seenEpoch) {
		std::ranges::fill(next.seen, 0u);
		next.seenEpoch = 1u;
	}
	next.casterMatches = std::move(draws.casterMatches);
	next.geometryOrdinals = std::move(draws.geometryOrdinals);
	next.materialOrdinals = std::move(draws.materialOrdinals);
	next.drawRecords = std::move(draws.drawRecords);
	next.projection = std::move(draws.projection);
	next.projectionTextures = std::move(draws.projectionTextures);
	next.geometryOrdinals.clear();
	next.materialOrdinals.clear();
	next.drawRecords.clear();
	next.projection.clear();
	next.projectionTextures.clear();
	draws = std::move(next);
	const auto frame = globals::game::graphicsState->GetFrameCount();
	const auto scene = acceleration.GetScene();
	const auto grid = globals::features::dx12Resources.GetGrid();
	draws.stamp.frame = frame;
	draws.active = true;
	if (grid && !grid->requested.empty()) {
		draws.stamp.scope = grid->requested.front().scope;
		draws.stamp.geometryRevision = grid->geometryRevision;
	}
	if (scene && scene->inputs && scene->scope == draws.stamp.scope && scene->geometryRevision == draws.stamp.geometryRevision) {
		draws.scene = scene;
		draws.seen.resize(scene->inputs->geometry.size());
		draws.stamp.materialRevision = scene->materialEvaluation ? scene->materialEvaluation->revision : 0;
		if (oldInputs != scene->inputs || draws.casterMatches.empty()) {
			draws.casterMatches.clear();
			draws.casterMatches.resize(FullNativeRasterDrawState::kCasterMatchSlots);
		}
	} else {
		draws.casterMatches.clear();
	}
	// One snapshot at the opaque boundary; subsequent state changes are observed
	// by binding detours. No per-draw OM/PS/IA queries.
	if (auto* ctx = globals::d3d::context) {
		std::array<ID3D11RenderTargetView*, 8> views{};
		winrt::com_ptr<ID3D11DepthStencilView> dsv;
		winrt::com_ptr<ID3D11PixelShader> ps;
		winrt::com_ptr<ID3D11BlendState> blend;
		winrt::com_ptr<ID3D11DepthStencilState> depth;
		winrt::com_ptr<ID3D11Buffer> index;
		DXGI_FORMAT format{};
		UINT mask = 0, stencil = 0;
		float factors[4]{};
		ctx->OMGetRenderTargets(UINT(views.size()), views.data(), dsv.put());
		ctx->PSGetShader(ps.put(), nullptr, nullptr);
		ctx->OMGetBlendState(blend.put(), factors, &mask);
		ctx->OMGetDepthStencilState(depth.put(), &stencil);
		ctx->IAGetPrimitiveTopology(&nativeInputTopology);
		ctx->IAGetIndexBuffer(index.put(), &format, &nativeInputIndexOffset);
		ObserveNativeInputIndexOffset(format, nativeInputIndexOffset);
		ObserveNativeInputTargets(UINT(views.size()), views.data(), dsv.get());
		ObserveNativeInputPS(ps.get());
		ObserveNativeInputBlend(blend.get(), mask);
		ObserveNativeInputDepth(depth.get());
		for (auto* view : views) if (view) view->Release();
	}
}

void DX12NativeScene::ObserveNativeInputTargets(UINT count, ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth)
{
	auto& draws = fullNativeRasterDraws;
	if (!draws.active || draws.finalized || count == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) return;
	draws.targetsBound = false;
	if (!views || count != 8 || !depth || !globals::game::renderer) return;
	auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	const RE::RENDER_TARGET expected[]{ RE::RENDER_TARGET::kMAIN, RE::RENDER_TARGET::kMOTION_VECTOR,
		NORMALROUGHNESS, ALBEDO, SPECULAR, REFLECTANCE, MASKS, MASKS2 };
	for (UINT i = 0; i < 8; ++i)
		if (!views[i] || views[i] != Util::AsReal(targets[expected[i]].RTV)) return;
	winrt::com_ptr<ID3D11Resource> boundDepth;
	depth->GetResource(boundDepth.put());
	const auto mainDepth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture;
	if (boundDepth.get() != Util::AsReal(mainDepth)) return;
	draws.targetsBound = true;
}

void DX12NativeScene::ObserveNativeInputBlend(ID3D11BlendState* state, UINT sampleMask)
{
	auto& draws = fullNativeRasterDraws;
	if (!draws.active || draws.finalized) return;
	draws.replacement = (sampleMask & 1u) != 0;
	if (!state) return;
	D3D11_BLEND_DESC desc{};
	state->GetDesc(&desc);
	draws.replacement &= !desc.AlphaToCoverageEnable;
	for (UINT i = 0; i < 8; ++i) {
		const auto& rt = desc.RenderTarget[desc.IndependentBlendEnable ? i : 0];
		draws.replacement &= !rt.BlendEnable && rt.RenderTargetWriteMask == D3D11_COLOR_WRITE_ENABLE_ALL;
	}
}

void DX12NativeScene::ObserveNativeInputDepth(ID3D11DepthStencilState* state)
{
	auto& draws = fullNativeRasterDraws;
	if (!draws.active || draws.finalized) return;
	D3D11_DEPTH_STENCIL_DESC desc{};
	if (state) state->GetDesc(&desc);
	draws.depthTest = !state || desc.DepthEnable;
}

void DX12NativeScene::ObserveNativeInputClear()
{
	if (fullNativeRasterDraws.knownWriters != 0) InvalidateNativeInputVisibility();
}

void DX12NativeScene::InvalidateNativeInputVisibility()
{
	auto& draws = fullNativeRasterDraws;
	if (draws.active && !draws.finalized) {
		draws.allWritersKnown = false;
		++draws.unknownWriters;
	}
}

void DX12NativeScene::ObserveFullNativeRasterDraw(RE::BSGeometry& geometry)
{
	using namespace DX12Native::MaterialFrameContract;
	if (!globals::deferred || !globals::deferred->deferredPass || !globals::game::graphicsState ||
		!globals::state || globals::state->isMapMenuOpen || globals::features::dx12Resources.IsLoading() || globals::state->nativeFirstPersonDepth ||
		(globals::state->permutationData.ExtraShaderDescriptor &
			static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return;
	const auto frame = globals::game::graphicsState->GetFrameCount();
	// Only known dynamic non-actor owners are admitted here. Actor attachments
	// are already captured by the actor path; static geometry must not be duplicated.
	if (characterCasters) {
		const auto& rt = geometry.GetGeometryRuntimeData();
		// Reject ineligible rigid movers before walking the live ancestry chain.
		if (geometry.AsTriShape() && !geometry.AsDynamicTriShape() && !rt.skinInstance && !rt.alphaProperty &&
			rt.rendererData && rt.rendererData->rawVertexData && rt.rendererData->rawIndexData) {
			CS_DRAW_ZONE("DX12::RigidMover::OwnerLookup");
			bool ancestryRead = false;
			const auto* reference = Util::FindSceneOwner(&geometry, false, &ancestryRead);
			if (ancestryRead && reference && reference->IsDynamicForm() && !reference->IsActor()) {
				const auto moverGrid = globals::features::dx12Resources.GetGrid();
				if (moverGrid && !moverGrid->requested.empty())
					characterCasters->ObserveRigidMover(geometry, frame, moverGrid->requested.front().scope);
			}
		}
	}
	auto& draws = fullNativeRasterDraws;
	if (!draws.active || draws.finalized || draws.stamp.frame != frame) return;
	++draws.draws;
	const auto& runtime = geometry.GetGeometryRuntimeData();
	const auto geometryKey = static_cast<const void*>(&geometry);
	const auto materialKey = static_cast<const void*>(runtime.shaderProperty.get());
	draws.activeGeometry = draws.geometryOrdinals.try_emplace(geometryKey, uint32_t(draws.geometryOrdinals.size()+1)).first->second;
	draws.activeMaterial = materialKey ? draws.materialOrdinals.try_emplace(materialKey, uint32_t(draws.materialOrdinals.size()+1)).first->second : 0;
	// A raster-only draw identity is assigned even without a scene/material generation.
	const auto scene = draws.scene;
	if (!scene || !scene->inputs || scene != acceleration.GetScene()) return;
	const auto* tri = geometry.AsTriShape();
	const auto& rt = geometry.GetGeometryRuntimeData();
	if (!tri || !rt.rendererData || rt.skinInstance || geometry.AsDynamicTriShape() ||
		!rt.shaderProperty || rt.shaderProperty->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get()) {
		++draws.rejected;
		return;
	}
	const auto* property = static_cast<const RE::BSLightingShaderProperty*>(rt.shaderProperty.get());
	using PropertyFlag = RE::BSShaderProperty::EShaderPropertyFlag;
	const bool projected = property->flags.any(PropertyFlag::kProjectedUV);
	if (projected) ++draws.projectedDraws;
	const auto counts = tri->GetTrishapeRuntimeData();
	auto desc = rt.vertexDesc;
	const DX12::CasterSource live{ reinterpret_cast<uintptr_t>(&geometry),
		reinterpret_cast<uintptr_t>(rt.rendererData->rawVertexData), reinterpret_cast<uintptr_t>(rt.rendererData->rawIndexData),
		counts.vertexCount, uint32_t(counts.triangleCount) * 3u, desc.GetSize(), 0 };
	std::array<float, 12> transform{};
	for (uint32_t row = 0; row < 3; ++row)
		for (uint32_t col = 0; col < 3; ++col)
			transform[4 * row + col] = geometry.world.rotate.entry[row][col] * geometry.world.scale;
	transform[3] = geometry.world.translate.x;
	transform[7] = geometry.world.translate.y;
	transform[11] = geometry.world.translate.z;
	UniqueRasterCandidate match;
	{
		CS_DRAW_ZONE("DX12::RasterInputs::CasterLookup");
		// Preserve exact source/transform checks and ambiguous-match rejection.
		// A hash collision only causes a miss; no engine pointer is dereferenced.
		auto* entry = draws.casterMatches.empty() ? nullptr :
			&draws.casterMatches[(live.geometry >> 4u) % draws.casterMatches.size()];
		if (entry && DX12::SameCasterSource(entry->source, live, entry->transform, transform)) {
			match = entry->match;
			++draws.matchHits;
		} else {
			++draws.matchMisses;
			const auto [first, rangeEnd] = scene->inputs->casterSources.equal_range(live.geometry);
			for (auto it = first; it != rangeEnd; ++it) {
				const auto ordinal = it->second;
				if (ordinal >= scene->inputs->geometry.size() || ordinal >= UINT32_MAX)
					continue;
				const auto& shape = scene->inputs->geometry[ordinal];
				if (shape && DX12::SameCasterSource(shape->casterSource, live, shape->transform, transform))
					match.Observe(uint32_t(ordinal));
			}
			if (entry) *entry = { live, transform, match };
		}
	}
	// Projection evidence remains independently stamped. A feature-limited
	// material may still publish raw geometry; shading eligibility is separate.
	if (match.Valid()) {
		auto& observation = draws.projection[match.ordinal];
		if (projected) {
			const auto& params = property->projectedUVParams;
			const auto& color = property->projectedUVColor;
			observation.Observe(true, { { params.red, params.green, params.blue, params.alpha },
				{ color.red, color.green, color.blue, color.alpha } });
			if (observation.ValidProjection())
				draws.activeProjectedInstance = match.ordinal;
		} else observation.Observe(false);
		// Conflicting live projection invalidates this instance's draw identity.
		if (observation.conflicted) {
			++draws.rejected;
			return;
		}
	} else if (projected) ++draws.unmatchedProjectedDraws;
	if (!match.Valid() || match.ordinal >= draws.seen.size()) {
		if (match.ambiguous)
			++draws.ambiguous;
		else
			++draws.rejected;
		return;
	}
	++draws.candidates;
	if (draws.seen[match.ordinal] != draws.seenEpoch) {
		draws.seen[match.ordinal] = draws.seenEpoch;
		++draws.uniqueInstances;
	}
	const auto primitiveCount = uint32_t(counts.triangleCount);
	if (!NativePBR::InputOnly::ValidIdentity(match.ordinal, primitiveCount) || match.ordinal >= 0x7ffffffeu) return;
	draws.activeSceneInstance = match.ordinal + 1u;
	draws.activePrimitiveCount = primitiveCount;
}

bool DX12NativeScene::CaptureProjectedConstants(ProjectedTextureObservation& observation)
{
	using namespace DX12Native::MaterialFrameContract;
	auto& draws = fullNativeRasterDraws;
	if (draws.constantSnapshots >= kMaxProjectedConstantSnapshots || !globals::d3d::device || !globals::d3d::context)
		return false;
	const auto& state = globals::game::shadowState->GetRuntimeData();
	if (!state.currentVertexShader || !state.currentPixelShader)
		return false;
	constexpr auto group = static_cast<size_t>(RE::BSGraphics::ConstantGroupLevel::PerGeometry);
	const auto& vs = *state.currentVertexShader;
	const auto& ps = *state.currentPixelShader;
	const auto& constants = ShaderConstants::LightingPS::Get();
	const std::array<uint32_t, 4> offsets{
		uint32_t(vs.constantTable[ShaderConstants::LightingTextureProj]) * uint32_t(sizeof(float)),
		uint32_t(ps.constantTable[constants.ProjectedUVParams]) * uint32_t(sizeof(float)),
		uint32_t(ps.constantTable[constants.ProjectedUVParams2]) * uint32_t(sizeof(float)),
		uint32_t(ps.constantTable[constants.ProjectedUVParams3]) * uint32_t(sizeof(float))
	};
	// TextureProj is a row-major float3x4; the PS fields are float4.
	// Zero offsets cannot identify these non-leading fields.
	if (std::ranges::any_of(offsets, [](uint32_t offset) { return offset == 0; }) ||
		offsets[1] == offsets[2] || offsets[1] == offsets[3] || offsets[2] == offsets[3])
		return false;
	std::array<ID3D11Buffer*, 2> sources{ Util::AsReal(vs.constantBuffers[group].buffer),
		Util::AsReal(ps.constantBuffers[group].buffer) };
	// Confirm the actual bound buffers agree with the engine shader groups.
	// Reject a stale group rather than copying an unrelated constant buffer.
	winrt::com_ptr<ID3D11Buffer> boundVS, boundPS;
	globals::d3d::context->VSGetConstantBuffers(uint32_t(group), 1, boundVS.put());
	globals::d3d::context->PSGetConstantBuffers(uint32_t(group), 1, boundPS.put());
	if (boundVS.get() != sources[0] || boundPS.get() != sources[1])
		return false;
	std::array<D3D11_BUFFER_DESC, 2> descriptions{};
	for (size_t i = 0; i < sources.size(); ++i) {
		if (!sources[i]) return false;
		sources[i]->GetDesc(&descriptions[i]);
		if (!(descriptions[i].BindFlags & D3D11_BIND_CONSTANT_BUFFER) ||
			descriptions[i].ByteWidth > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16u)
			return false;
	}
	for (size_t i = 0; i < offsets.size(); ++i)
		if (!ValidProjectionConstantExtent(offsets[i], i == 0 ? 48u : 16u, descriptions[i == 0 ? 0 : 1].ByteWidth))
			return false;
	if (projectedConstantDevice.get() != globals::d3d::device) {
		projectedConstantPool = {};
		projectedConstantDevice.copy_from(globals::d3d::device);
	}
	auto& slot = projectedConstantPool[draws.constantSnapshots];
	for (size_t i = 0; i < sources.size(); ++i) {
		if (!slot.buffers[i] || !slot.views[i] || slot.byteWidths[i] != descriptions[i].ByteWidth) {
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = descriptions[i].ByteWidth;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			winrt::com_ptr<ID3D11Buffer> replacement;
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, replacement.put())))
				return false;
			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
			viewDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			viewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
			viewDesc.BufferEx.NumElements = desc.ByteWidth / sizeof(uint32_t);
			viewDesc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
			winrt::com_ptr<ID3D11ShaderResourceView> replacementView;
			if (FAILED(globals::d3d::device->CreateShaderResourceView(replacement.get(), &viewDesc, replacementView.put())))
				return false;
			slot.buffers[i] = std::move(replacement);
			slot.views[i] = std::move(replacementView);
			slot.byteWidths[i] = desc.ByteWidth;
		}
	}
	// CopyResource preserves the submitted GPU buffer version and does not
	// access the engine's invalidated mapped pointer. Both copies are queued
	// before the next draw can replace the engine geometry constants.
	for (size_t i = 0; i < sources.size(); ++i)
		globals::d3d::context->CopyResource(slot.buffers[i].get(), sources[i]);
	observation.constantSlot = draws.constantSnapshots++;
	observation.constantOffsets = offsets;
	return true;
}

void DX12NativeScene::ObserveProjectedDrawBindings(const RE::BSRenderPass* pass)
{
	// The render-pass scope is the sole admission path. No D3D state reads
	// or retained COM references are needed for unrelated draws.
	auto& draws = fullNativeRasterDraws;
	const auto ordinal = draws.activeProjectedInstance;
	if (ordinal == UINT32_MAX || draws.finalized || !pass || !pass->geometry ||
		!globals::features::dx12Control.WantsMaterialPreparation() || !globals::game::shadowState ||
		!globals::game::graphicsState || draws.stamp.frame != globals::game::graphicsState->GetFrameCount() ||
		!draws.scene || !draws.scene->inputs || ordinal >= draws.scene->inputs->geometry.size())
		return;
	const auto& shape = draws.scene->inputs->geometry[ordinal];
	if (!shape || shape->casterSource.geometry != reinterpret_cast<uintptr_t>(pass->geometry))
		return;
	const auto property = draws.projection.find(ordinal);
	if (property == draws.projection.end() || !property->second.ValidProjection())
		return;
	const auto& state = globals::game::shadowState->GetRuntimeData();
	if (!state.currentPixelShader)
		return;
	using Cache = SIE::ShaderCache;
	using Flags = Cache::LightingShaderFlags;
	const auto descriptor = state.currentPixelShader->id;
	// Restrict to the ordinary object permutation. Other techniques alias
	// the same slots for unrelated resources (parallax, sparkle, LOD).
	if (!(descriptor & static_cast<uint32_t>(Flags::ProjectedUV)) ||
		(descriptor & static_cast<uint32_t>(Flags::Skinned)) ||
		Cache::GetLightingTechnique(descriptor) != Cache::LightingShaderTechniques::None)
		return;
	constexpr std::array<uint32_t, 4> slots{ 3, 8, 10, 11 };
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, slots.size()> views;
	uint32_t samplerModes = 0;
	for (size_t i = 0; i < slots.size(); ++i) {
		samplerModes |= (static_cast<uint32_t>(state.PSTextureAddressMode[slots[i]].get()) & 3u) << (2u * i);
		auto* view = Util::AsReal(state.PSTexture[slots[i]]);
		if (view) {
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			view->GetDesc(&desc);
			if (desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || desc.Format == DXGI_FORMAT_UNKNOWN)
				return;
			views[i].copy_from(view);
			if (i == 0 && DirectX::IsSRGB(desc.Format)) samplerModes |= 256u;
		}
	}
	// Noise is required even for the solid-color projection permutation.
	if (!views.back())
		return;
	auto& observation = draws.projectionTextures[ordinal];
	if (observation.conflicted)
		return;
	if (observation.observed) {
		// Equal texture identities do not prove equal per-draw constants.
		// Do not certify a repeated instance until GPU equality is implemented.
		observation.repeatedConstants = true;
		bool same = observation.descriptor == descriptor && observation.samplerModes == samplerModes;
		for (size_t i = 0; i < views.size(); ++i)
			same = same && observation.views[i].get() == views[i].get();
		if (!same) {
			observation.conflicted = true;
			observation.views = {};
		}
		return;
	}
	observation.descriptor = descriptor;
	observation.samplerModes = samplerModes;
	observation.views = std::move(views);
	observation.observed = true;
	if (!CaptureProjectedConstants(observation))
		++draws.constantSnapshotFailures;
}

void DX12NativeScene::StampFullNativeIndexedDraw(uint32_t count, uint32_t start, int32_t base, bool indexed, uint32_t instances, uint32_t firstInstance)
{
	auto* state = globals::state;
	auto& draws = fullNativeRasterDraws;
	if (!draws.active || draws.finalized || !state || !globals::game::graphicsState ||
		draws.stamp.frame != globals::game::graphicsState->GetFrameCount()) return;
	auto& data = state->permutationData;
	data.nativeRasterInstance = data.nativeRasterPrimitiveCount = 0;
	data.nativeRasterMaterialIndex = UINT32_MAX;
	using Type = RE::BSShader::Type;
	using Cache = SIE::ShaderCache;
	const auto descriptor = state->modifiedPixelDescriptor;
	bool writer = false;
	bool attributes = false;
	if (state->currentShader && state->customPixelShader &&
		nativeInputPS == Util::AsReal(state->customPixelShader->shader)) {
		switch (state->currentShader->shaderType.get()) {
		case Type::Lighting:
			writer = attributes = (descriptor & uint32_t(Cache::LightingShaderFlags::Deferred)) != 0;
			break;
		case Type::DistantTree:
			writer = attributes = (descriptor & uint32_t(Cache::DistantTreeShaderFlags::Deferred)) != 0 && !(descriptor & 1u);
			break;
		case Type::Effect:
			writer = (descriptor & uint32_t(Cache::EffectShaderFlags::Deferred)) != 0;
			break;
		case Type::Grass: {
			const auto technique = descriptor & 15u;
			writer = technique != uint32_t(Cache::GrassShaderTechniques::RenderDepth);
			break; // Grass currently explicitly invalidates its identity.
		}
		default: break; // Unverified permutations must not certify visibility.
		}
	}
	const bool coherent = writer && draws.targetsBound && draws.replacement && draws.depthTest;
	if (coherent) ++draws.knownWriters;
	else InvalidateNativeInputVisibility();
	if (!coherent || !attributes || !draws.activeGeometry || !count ||
		nativeInputTopology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST || !instances || count % 3u) {
		state->UpdatePermutationBuffer();
		return;
	}
	// Partial ranges keep their own raster identity. Only an entire matched
	// triangle list with an unshifted index binding maps to a TLAS ordinal.
	const bool exact = indexed && instances == 1 && firstInstance == 0 &&
		draws.activeSceneInstance && start == 0 && base == 0 && nativeInputIndexOffset == 0 && nativeInputIndexElementBytes != 0 &&
		NativePBR::InputOnly::WholeTriangleList(count, draws.activePrimitiveCount);
	if (instances != 1 || draws.nextRasterDraw >= 0x7ffffffeu) {
		// No instance-interpolant contract yet: explicitly invalidate.
		state->UpdatePermutationBuffer();
		return;
	}
	const uint32_t encoded = uint32_t(draws.drawRecords.size()+1u);
	++draws.nextRasterDraw;
	data.nativeRasterInstance = encoded;
	data.nativeRasterPrimitiveCount = count / 3u;
	const uint32_t sceneInstance = exact ? draws.activeSceneInstance-1u : UINT32_MAX;
	if (exact && draws.scene && draws.scene->materialEvaluation) {
		const auto& indices = draws.scene->materialEvaluation->rasterMaterialIndices;
		if (sceneInstance < indices.size()) data.nativeRasterMaterialIndex = indices[sceneInstance];
	}
	draws.drawRecords.push_back({ encoded, draws.activeGeometry, draws.activeMaterial, sceneInstance,
		count/3u, 0u, data.nativeRasterMaterialIndex, count, start, nativeInputIndexOffset, nativeInputIndexElementBytes,
		base, descriptor, state->modifiedVertexDescriptor, data.ExtraShaderDescriptor, data.ExtraFeatureDescriptor,
		instances, firstInstance, indexed ? 1u : 0u, 0u });
	state->UpdatePermutationBuffer();
}

void DX12NativeScene::FinishFullNativeRasterDraws()
{
	ZoneScopedN("DX12::RasterInputs::Publish");
	if (!globals::game::graphicsState)
		return;
	auto& draws = fullNativeRasterDraws;
	if (!draws.active || draws.finalized) return;
	if (draws.stamp.frame != globals::game::graphicsState->GetFrameCount()) return;
	draws.finalized = true;
	draws.active = false;
	if (!backend) {
		std::string reason;
		backend = GraphicsBackend::Create(globals::d3d::device, globals::d3d::context, reason);
	}
	if (backend && globals::game::renderer) {
		auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
		std::array<ID3D11Texture2D*, NativePBR::InputOnly::kPlaneCount> inputs{
			Util::AsReal(targets[ALBEDO].texture), Util::AsReal(targets[SPECULAR].texture),
			Util::AsReal(targets[REFLECTANCE].texture), Util::AsReal(targets[MASKS].texture) };
		const auto world = globals::state ? GetWorldFrame(draws.stamp.frame, globals::state->frameCount) : nullptr;
		rawInputCapture.Publish(*backend, Util::AsReal(targets[MASKS2].texture), inputs, draws.stamp, draws.drawRecords, draws.draws,
			world ? world->width : 0u, world ? world->height : 0u,
			draws.allWritersKnown && draws.knownWriters != 0 && world != nullptr);
	}
	// Projection consumers may use only matching scene generations.
	if (!draws.scene || draws.scene != acceleration.GetScene()) return;
	if (backend && draws.constantSnapshots) {
		std::vector<DX12Native::ProjectedInputDraw> inputs;
		inputs.reserve(draws.constantSnapshots);
		for (const auto& [ordinal, observation] : draws.projectionTextures) {
			const auto property = draws.projection.find(ordinal);
			if (property == draws.projection.end() || !property->second.ValidProjection() ||
				observation.conflicted || observation.repeatedConstants || observation.constantSlot >= draws.constantSnapshots)
				continue;
			const auto& slot = projectedConstantPool[observation.constantSlot];
			if (!slot.views[0] || !slot.views[1]) continue;
			inputs.push_back({ ordinal, observation.descriptor, observation.samplerModes, { slot.views[0].get(), slot.views[1].get() },
				observation.constantOffsets, observation.views });
		}
		if (!inputs.empty()) {
			globals::features::nativePBR.PublishProjection(*backend, draws.stamp, inputs);
		}
	}
}

std::function<void()> DX12NativeScene::OnRenderPassBegin(const RE::BSRenderPass* pass)
{
	if (loaded && !globals::game::isVR && globals::features::dx12Control.WantsMaterialPreparation()) {
		if (!globals::state)
			return nullptr;
		auto& data = globals::state->permutationData;
		const auto oldBase = data.nativeRasterInstance;
		const auto oldCount = data.nativeRasterPrimitiveCount;
		const auto oldMaterialIndex = data.nativeRasterMaterialIndex;
		const auto oldGeometry = fullNativeRasterDraws.activeGeometry, oldMaterial = fullNativeRasterDraws.activeMaterial;
		const auto oldSceneInstance = fullNativeRasterDraws.activeSceneInstance, oldPrimitiveCount = fullNativeRasterDraws.activePrimitiveCount;
		const auto oldProjection = fullNativeRasterDraws.activeProjectedInstance;
		const auto oldStamp = fullNativeRasterDraws.stamp;
		fullNativeRasterDraws.activeProjectedInstance = UINT32_MAX;
		data.nativeRasterInstance = data.nativeRasterPrimitiveCount = 0;
		data.nativeRasterMaterialIndex = UINT32_MAX;
		fullNativeRasterDraws.activeGeometry = fullNativeRasterDraws.activeMaterial = 0;
		fullNativeRasterDraws.activeSceneInstance = fullNativeRasterDraws.activePrimitiveCount = 0;
		if (pass && pass->geometry)
			ObserveFullNativeRasterDraw(*pass->geometry);
		return [this, oldBase, oldCount, oldMaterialIndex, oldGeometry, oldMaterial, oldSceneInstance, oldPrimitiveCount, oldProjection, oldStamp] {
			fullNativeRasterDraws.activeProjectedInstance =
				fullNativeRasterDraws.stamp == oldStamp && !fullNativeRasterDraws.finalized ? oldProjection : UINT32_MAX;
			const bool same = fullNativeRasterDraws.stamp == oldStamp && !fullNativeRasterDraws.finalized;
			fullNativeRasterDraws.activeGeometry = same ? oldGeometry : 0;
			fullNativeRasterDraws.activeMaterial = same ? oldMaterial : 0;
			fullNativeRasterDraws.activeSceneInstance = same ? oldSceneInstance : 0;
			fullNativeRasterDraws.activePrimitiveCount = same ? oldPrimitiveCount : 0;
			if (globals::state) {
				globals::state->permutationData.nativeRasterInstance = same ? oldBase : 0;
				globals::state->permutationData.nativeRasterPrimitiveCount = same ? oldCount : 0;
				globals::state->permutationData.nativeRasterMaterialIndex = same ? oldMaterialIndex : UINT32_MAX;
			}
		};
	}
	// Observe visible static geometry already drawn by the existing opaque
	// render path. This does not change render-pass state or DX11 draw behavior.
	if (!loaded || !globals::features::dx12Control.IsActive() || globals::game::isVR ||
		!pass || !pass->geometry ||
		GetFrameNativeMode() == DX12::SCMNativeMode::Disabled ||
		!globals::features::upscaling.loaded ||
		!globals::state || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor &
			static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return nullptr;
	ObserveSCMRigidCaster(*pass->geometry, true);
	return nullptr;
}

void DX12NativeScene::ObserveSCMRigidCaster(RE::BSGeometry& geometry, bool requireStability)
{
	// SCM and already-submitted opaque render passes share this bounded
	// CPU-only capture path. No scene traversal, readback or GPU wait.
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive() ||
		GetFrameNativeMode() == DX12::SCMNativeMode::Disabled)
		return;
	auto* tri = geometry.AsTriShape();
	if (!tri || geometry.AsDynamicTriShape())
		return;
	const auto& runtime = geometry.GetGeometryRuntimeData();
	if (runtime.skinInstance || runtime.alphaProperty || !runtime.rendererData ||
		!runtime.rendererData->rawVertexData || !runtime.rendererData->rawIndexData)
		return;
	// Ignore effects, sky, water and other non-lighting draws; geometry only.
	const auto* property = runtime.shaderProperty.get();
	if (!property || property->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
		return;
	// A drawn BSTriShape often has no user data of its own; Skyrim keeps the
	// placed reference on an ancestor NiNode. Walk only the existing parent chain.
	bool ancestryRead = false;
	const RE::TESForm* reference = Util::FindSceneOwner(&geometry, false, &ancestryRead);
	if (!ancestryRead || (reference && (reference->IsDynamicForm() || reference->IsActor())))
		return;
	const auto* file = reference ? reference->GetFile(0) : nullptr;
	auto desc = runtime.vertexDesc;
	if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
		return;
	const auto counts = tri->GetTrishapeRuntimeData();
	const uint32_t stride = desc.GetSize();
	if (stride < 16 || stride > 256 || !counts.vertexCount || !counts.triangleCount ||
		counts.vertexCount > 65535 || counts.triangleCount > 65535)
		return;

	DX12Native::RuntimeGeometryCandidate candidate;
	candidate.referenceKey = reference && file && !file->GetFilename().empty() ?
	                             std::format("{:06X}:{}", reference->GetLocalFormID(), file->GetFilename()) :
	                             std::format("render:{:016X}", reinterpret_cast<std::uintptr_t>(&geometry));
	candidate.shapeName = geometry.name.empty() ? "<unnamed>" : geometry.name.c_str();
	candidate.requiresStability = requireStability;
	candidate.objectIdentity = reinterpret_cast<std::uintptr_t>(&geometry);
	candidate.stride = stride;
	candidate.vertexCount = counts.vertexCount;
	candidate.triangleCount = counts.triangleCount;
	candidate.vertices = runtime.rendererData->rawVertexData;
	candidate.indices = runtime.rendererData->rawIndexData;
	for (uint32_t row = 0; row < 3; ++row)
		for (uint32_t col = 0; col < 3; ++col)
			candidate.transform[4 * row + col] = geometry.world.rotate.entry[row][col] * geometry.world.scale;
	candidate.transform[3] = geometry.world.translate.x;
	candidate.transform[7] = geometry.world.translate.y;
	candidate.transform[11] = geometry.world.translate.z;
	candidate.centerX = geometry.worldBound.center.x;
	candidate.centerY = geometry.worldBound.center.y;
	const uint32_t frame = globals::game::graphicsState ?
	                           globals::game::graphicsState->GetFrameCount() :
	                           0;
	geometryCapture.Observe(candidate, frame, globals::features::dx12Resources.GetGrid());
}

void DX12NativeScene::OnLoadingMenuEvent(bool opening)
{
	if (opening)
		loadingResetPending.store(true, std::memory_order_release);
}

void DX12NativeScene::OnSceneTransitionReset(bool a_opening)
{
	if (a_opening)
		ApplyPendingLoadingReset();
}

void DX12NativeScene::ApplyPendingLoadingReset()
{
	if (!loadingResetPending.exchange(false, std::memory_order_acq_rel))
		return;
	frameNativeModeID.store(UINT32_MAX, std::memory_order_release);
	frameNativeMode.store(DX12::SCMNativeMode::Disabled, std::memory_order_release);
	if (nativeSessionStarted) {
		shadowLightScope.store(0, std::memory_order_release);
		shadowLights.store(nullptr, std::memory_order_release);
		{
			std::scoped_lock lock(runtimeLightMutex);
			runtimeLightFrame = UINT32_MAX;
			runtimeLightSamples.clear();
			ResetFullLightInventory();
		}
		globals::features::upscaling.nativeComposite.ResetShadowHistory();
		fullNativeRasterDraws = {};
		rawInputCapture.Reset();
		if (characterCasters) characterCasters->Reset();
		ResetFullNativeEnvironment();
		if (fullNativeDirect)
			fullNativeDirect->Reset();
		if (globals::features::dx12Control.UsesExclusiveMaterials()) {
			globals::features::nativePBR.InvalidateGeneration();
			globals::features::nativeLLF.ResetParticleInput();
		}
		raster.Reset();
		shadows.Reset();
		acceleration.Reset();
		geometryCapture.Reset();
	}
}

void DX12NativeScene::OnRuntimeDisabled()
{
	rawInputCapture.Reset();
	projectedConstantPool = {};
	projectedConstantDevice = nullptr;
	if (globals::features::dx12Control.UsesExclusiveMaterials())
		globals::features::nativePBR.ReleaseProjection();
	characterCasters.reset();
	loadingResetPending.store(false, std::memory_order_release);
	frameNativeModeID.store(UINT32_MAX, std::memory_order_release);
	frameNativeMode.store(DX12::SCMNativeMode::Disabled, std::memory_order_release);
	frameExactShadowLights.store(false, std::memory_order_relaxed);
	movingOriginCount.store(0, std::memory_order_relaxed);
	if (!nativeSessionStarted) {
		if (globals::features::dx12Control.UsesExclusiveMaterials()) {
			globals::features::nativeLLF.OnRuntimeDisabled();
			globals::features::nativeIBL.OnRuntimeDisabled();
			globals::features::nativeCubemaps.OnRuntimeDisabled();
			globals::features::nativePBR.OnRuntimeDisabled();
		}
		return;
	}
	nativeSessionStarted = false;
	shadowLightScope.store(0, std::memory_order_release);
	shadowLights.store(nullptr, std::memory_order_release);
	{
		std::scoped_lock lock(runtimeLightMutex);
		runtimeLightFrame = UINT32_MAX;
		runtimeLightSamples.clear();
		ResetFullLightInventory();
	}
	globals::features::upscaling.nativeComposite.ResetShadowHistory();
	fullNativeRasterDraws = {};
	ResetFullNativeEnvironment();
	fullNativeDirect.reset();
	if (globals::features::dx12Control.UsesExclusiveMaterials()) {
		globals::features::nativeIBL.Release();
		globals::features::nativeCubemaps.Release();
		globals::features::nativePBR.Release();
		globals::features::nativeLLF.OnRuntimeDisabled();
	}
	raster.Reset();
	shadows.Reset();
	acceleration.Stop();
	geometryCapture.Reset();
}

bool DX12NativeScene::CasterInTlas(RE::BSGeometry& geometry) const
{
	const auto scene = acceleration.GetScene();
	if (!scene || !scene->inputs || scene->scope != shadowLightScope.load(std::memory_order_acquire))
		return false;
	const auto* tri = geometry.AsTriShape();
	const auto& rt = geometry.GetGeometryRuntimeData();
	if (!tri || !rt.rendererData || rt.skinInstance || rt.alphaProperty || geometry.AsDynamicTriShape())
		return false;
	const auto counts = tri->GetTrishapeRuntimeData();
	auto desc = rt.vertexDesc;
	const DX12::CasterSource live{ reinterpret_cast<uintptr_t>(&geometry),
		reinterpret_cast<uintptr_t>(rt.rendererData->rawVertexData), reinterpret_cast<uintptr_t>(rt.rendererData->rawIndexData),
		counts.vertexCount, uint32_t(counts.triangleCount) * 3u, desc.GetSize(), 0 };
	std::array<float, 12> transform{};
	for (uint32_t row = 0; row < 3; ++row)
		for (uint32_t col = 0; col < 3; ++col)
			transform[4 * row + col] = geometry.world.rotate.entry[row][col] * geometry.world.scale;
	transform[3] = geometry.world.translate.x;
	transform[7] = geometry.world.translate.y;
	transform[11] = geometry.world.translate.z;
	const auto [first, rangeEnd] = scene->inputs->casterSources.equal_range(live.geometry);
	for (auto it = first; it != rangeEnd; ++it) {
		const auto& shape = scene->inputs->geometry[it->second];
		if (DX12::SameCasterSource(shape->casterSource, live, shape->transform, transform))
			return true;
	}
	return false;
}

void DX12NativeScene::AuditCasterCoverage(RE::BSGeometry& geometry) const
{
	if (!coverageAudit.load(std::memory_order_relaxed) || !globals::state || !globals::state->IsDeveloperMode())
		return;
	(CasterInTlas(geometry) ? coveredCasterChecks : missingCasterChecks).fetch_add(1, std::memory_order_relaxed);
}

json DX12NativeScene::GetStatus() const
{
	json status = { { "active", globals::features::dx12Control.IsActive() },
		{ "shadowOffloadMode", globals::features::dx12Control.IsShadowOffloadMode() },
		{ "frameNativeMode", magic_enum::enum_name(GetFrameNativeMode()) },
		{ "perLightVisibility", false } };
	if (globals::features::dx12Control.UsesExclusiveMaterials()) {
		const auto world = worldFrame.load(std::memory_order_acquire);
		status["worldCamera"] = { { "engineFrame", world ? world->engineFrame : 0u },
			{ "pluginFrame", world ? world->pluginFrame : 0u }, { "identity", world ? world->cameraIdentity : 0u },
			{ "width", world ? world->width : 0u }, { "height", world ? world->height : 0u },
			{ "jitterX", world ? world->jitter.x : 0.0f }, { "jitterY", world ? world->jitter.y : 0.0f },
			{ "rejectedUploads", rejectedWorldCameraUploads } };
	}
	if (const auto grid = globals::features::dx12Resources.GetGrid()) {
		json active = json::array();
		json predictive = json::object();
		for (const auto& tile : grid->resident) {
			if (!tile) {
				active.push_back(nullptr);
				continue;
			}
			json rejections = json::array();
			for (const auto& [reason, count] : tile->rejectionReasons)
				rejections.push_back({ { "reason", reason }, { "count", count } });
			active.push_back({ { "x", tile->key.x }, { "y", tile->key.y }, { "state", magic_enum::enum_name(tile->state) },
				{ "shapes", tile->shapes.size() }, { "lights", tile->lights.size() }, { "bytes", tile->bytes },
				{ "failure", tile->failureReason }, { "rejections", std::move(rejections) } });
		}
		for (const auto& tile : grid->predictiveResident) {
			const auto state = tile ? std::string(magic_enum::enum_name(tile->state)) : "Missing";
			predictive[state] = predictive.value(state, 0) + 1;
		}
		status["grid"] = { { "scope", grid->requested.front().scope }, { "size", grid->size },
			{ "revision", grid->geometryRevision }, { "runtimeShapes", grid->runtimeShapes.size() },
			{ "activeTiles", std::move(active) }, { "predictiveStates", std::move(predictive) } };
	}
	if (const auto scene = acceleration.GetScene(); scene && scene->acceleration && scene->inputs) {
		status["tlas"] = { { "published", true }, { "scope", scene->scope }, { "revision", scene->geometryRevision },
			{ "instances", scene->inputs->geometry.size() }, { "preparedShapes", scene->inputs->preparedShapes },
			{ "observedShapes", scene->inputs->observedShapes }, { "duplicateShapes", scene->inputs->duplicateShapes },
			{ "triangles", scene->inputs->triangles } };
	} else {
		status["tlas"] = { { "published", false } };
	}
	if (globals::features::dx12Control.WantsMaterialPreparation()) {
		const auto& draws = fullNativeRasterDraws;
		uint32_t validProjection = 0, conflictingProjection = 0;
		for (const auto& [ordinal, observation] : draws.projection) {
			validProjection += observation.ValidProjection() ? 1u : 0u;
			conflictingProjection += observation.conflicted ? 1u : 0u;
		}
		uint32_t selectedTextureInstances = 0, completeTextureViews = 0, conflictingTextures = 0, uniqueConstantSnapshots = 0;
		for (const auto& [ordinal, observation] : draws.projectionTextures) {
			const auto property = draws.projection.find(ordinal);
			if (property == draws.projection.end() || !property->second.ValidProjection())
				continue;
			if (observation.conflicted) ++conflictingTextures;
			else if (observation.observed) {
				++selectedTextureInstances;
				if (observation.constantSlot != UINT32_MAX && !observation.repeatedConstants) ++uniqueConstantSnapshots;
				if (std::ranges::all_of(observation.views, [](const auto& view) { return bool(view); }))
					++completeTextureViews;
			}
		}
		const auto projectionFrame = globals::features::nativePBR.GetProjection(draws.stamp);
		status["fullNativeProjectionInputs"] = { { "frame", draws.stamp.frame }, { "finalized", draws.finalized },
			{ "projectedDraws", draws.projectedDraws }, { "unmatchedDraws", draws.unmatchedProjectedDraws },
			{ "validPropertyInstances", validProjection }, { "conflictingInstances", conflictingProjection },
			{ "selectedTextureInstances", selectedTextureInstances }, { "completeTextureViews", completeTextureViews },
			{ "conflictingTextureInstances", conflictingTextures },
			{ "constantSnapshots", draws.constantSnapshots }, { "uniqueConstantSnapshots", uniqueConstantSnapshots },
			{ "constantSnapshotFailures", draws.constantSnapshotFailures },
			{ "projectorConstantsReady", projectionFrame != nullptr },
			{ "publishedRows", projectionFrame ? projectionFrame->rowCount : 0u },
			{ "residentProjectedTextures", projectionFrame ? projectionFrame->textures.size() : 0u },
			{ "missingProjectedTextures", projectionFrame ? projectionFrame->missingTextures : 0u },
			{ "projectionTransfers", projectionFrame ? projectionFrame->transferredTextures : 0u },
			{ "projectionNewTextures", projectionFrame ? projectionFrame->newTextures : 0u },
			{ "projectionCacheHits", projectionFrame ? projectionFrame->cacheHits : 0u },
			{ "projectionSuppressedTextures", projectionFrame ? projectionFrame->suppressedTextures : 0u },
			{ "projectionDeferredTextures", projectionFrame ? projectionFrame->deferredTextures : 0u },
			{ "projectionSlotBytes", projectionFrame ? projectionFrame->allocationBytes : 0ull },
			{ "projectionImmutableCacheBytes", projectionFrame ? projectionFrame->immutableCacheBytes : 0ull },
			{ "projectedTexturesReady", projectionFrame && !projectionFrame->missingTextures } };
		status["fullNativeRasterAdmission"] = { { "frame", draws.stamp.frame }, { "scope", draws.stamp.scope },
			{ "geometryRevision", draws.stamp.geometryRevision }, { "materialRevision", draws.stamp.materialRevision },
			{ "draws", draws.draws }, { "candidateDraws", draws.candidates }, { "uniqueInstances", draws.uniqueInstances },
			{ "ambiguousDraws", draws.ambiguous }, { "rejectedDraws", draws.rejected }, { "finalized", draws.finalized },
			{ "casterLookupHits", draws.matchHits }, { "casterLookupMisses", draws.matchMisses },
			{ "integerIdentity", true }, { "rasterDrawRecords", draws.drawRecords.size() },
			{ "rawInputPublished", bool(rawInputCapture.GetForFrame(draws.stamp.frame)) },
			{ "rawInputFailure", rawInputCapture.Failure() },
			{ "knownVisibilityWriters", draws.knownWriters }, { "unknownVisibilityWriters", draws.unknownWriters },
			{ "exactPixelIdentity", rawInputCapture.GetForFrame(draws.stamp.frame) &&
				rawInputCapture.GetForFrame(draws.stamp.frame)->evidence->winnerCertified }, { "rasterReplacement", false } };
		const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : UINT32_MAX;
		const auto characters = characterCasters ? characterCasters->Get(frame, draws.stamp.scope) : nullptr;
		const auto characterView = characters ? characters->Reflections() : DX12Native::CharacterCasterFrame::ReflectionView{};
		const auto* characterMaterials = characterView.evaluation;
		status["fullNativeCharacterCasters"] = { { "submitted", characters != nullptr },
			{ "reflectionMaterialsReady", characterMaterials != nullptr },
			{ "reflectionRigidInstances", characterView.moverCount },
			{ "materialGeneration", characterMaterials ? characterMaterials->revision : 0ull },
			{ "materialParts", characterMaterials ? characterMaterials->instanceCount : 0u },
			{ "residentRecipes", characterMaterials ? characterMaterials->residentRecipes : 0u },
			{ "featureLimitedParts", characterMaterials ? characterMaterials->baseOnlyBindings : 0u },
			{ "failedTextures", characterMaterials ? characterMaterials->failedTextures : 0u },
			{ "materialTextureBytes", characterMaterials ? characterMaterials->textureBytes : 0ull },
			{ "updateInterval", characterUpdateInterval }, { "rigidInstances", characters ? characters->rigidInstances : 0u },
			{ "actorAge", characters ? frame - (characters->actorSource ? characters->actorSource->frame : characters->frame) : 0u },
			{ "parts", characters ? characters->parts : 0u }, { "vertices", characters ? characters->vertices : 0u },
			{ "triangles", characters ? characters->triangles : 0u },
			{ "rejectedParts", characterCasters ? characterCasters->RejectedParts() : 0u },
			{ "failure", characterCasters ? characterCasters->Failure() : "" } };
		const auto reconstructed = globals::features::upscaling.nativeComposite.GetFullNativeReconstruction(frame);
		const auto finalColor = globals::features::upscaling.nativeComposite.GetFullNativeFinalColor(frame);
		status["fullNativeComposite"] = { { "reconstructed", reconstructed != nullptr },
			{ "availableContributions", reconstructed ? reconstructed->available : 0u },
			{ "winnerCertified", reconstructed && reconstructed->rasterWinnerCertified },
			{ "completeLightingCoverage", reconstructed && reconstructed->completeLightingCoverage },
			{ "finalColorPublished", finalColor != nullptr }, { "applicationEligible", finalColor && finalColor->applicationEligible },
			{ "applicationSubmitted", finalColor && finalColor->applied } };
	}
	status["ownership"] = { { "granted", ownershipGranted.load(std::memory_order_relaxed) },
		{ "coveredCasterChecks", coveredCasterChecks.load(std::memory_order_relaxed) },
		{ "missingCasterChecks", missingCasterChecks.load(std::memory_order_relaxed) } };
	status["trace"] = { { "lastOutcome", magic_enum::enum_name(shadows.LastOutcome()) }, { "lights", shadows.LastLightCount() },
		{ "movingOrigins", movingOriginCount.load(std::memory_order_relaxed) },
		{ "producedVisibilitySlices", shadows.LastVisibilityCount() } };
	status["composite"] = { { "lastShadowResult", magic_enum::enum_name(DX12Native::lastCompositeShadowResult.load(std::memory_order_relaxed)) } };
	return status;
}

void DX12NativeScene::RegisterUxActions()
{
	FEATURE_COMMAND("setMaskDebugView", "Developer-only native visibility preview. Params: enabled (bool).",
		[](Feature*, const json& args) {
			if (globals::state && globals::state->IsDeveloperMode())
				DX12Native::compositeShadowDebugView.store(args.value("enabled", false), std::memory_order_relaxed);
		});
	FEATURE_RENDER_QUERY("status", "Native shadow pipeline readiness, grid and published TLAS counts. Params: none.",
		[](const Feature* self, const json&) -> json { return static_cast<const DX12NativeScene*>(self)->GetStatus(); });
}

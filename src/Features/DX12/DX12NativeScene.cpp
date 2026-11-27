#include "DX12NativeScene.h"
#include "DX12LightMath.h"
#include "Features/DynamicCubemaps.h"
#include "Features/IBL.h"
#include "Features/Skylighting.h"
#include "Features/ExtendedMaterials.h"
#include "Features/FoliageLighting.h"
#include "Features/LinearLighting.h"
#include "Features/CSUtility.h"

#include "../LightLimitFix/ShadowCasterMath.h"
#include "../LightLimitFix/Common.h"
#include "../LightLimitFix/RadiusMath.h"
#include "../Upscaling.h"
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
	if (control.IsFullRTMode()) {
		const char* qualities[]{ T("menu.dx12.scene.rt_quality", "Quality"), T("menu.dx12.scene.rt_performance", "Performance") };
		int quality = fullRTQuality == DX12Native::kShadowQualityPerformance ? 1 : 0;
		if (ImGui::Combo(T("menu.dx12.scene.shadow_quality", "Shadow quality"), &quality, qualities, int(std::size(qualities))))
			fullRTQuality = quality == 0 ? DX12Native::kShadowQualityQuality : DX12Native::kShadowQualityPerformance;
		bool outdoor = outdoorFullRT.load(std::memory_order_relaxed);
		if (ImGui::Checkbox(T("menu.dx12.scene.outdoor_shadow_offloading", "Enable outdoor shadow offloading (DX12 only)"), &outdoor))
			outdoorFullRT.store(outdoor, std::memory_order_relaxed);
	} else if (control.IsShadowOffloadMode()) {
		bool outdoor = outdoorShadowOffloading.load(std::memory_order_relaxed);
		if (ImGui::Checkbox(T("menu.dx12.scene.outdoor_shadow_offloading", "Enable outdoor shadow offloading (DX12 only)"), &outdoor))
			outdoorShadowOffloading.store(outdoor, std::memory_order_relaxed);
	}
}

void DX12NativeScene::DrawDevSettings()
{
	const auto& control = globals::features::dx12Control;
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
	if (!earlyTraceExperiment.load(std::memory_order_relaxed) || !globals::state || !globals::state->IsDeveloperMode())
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
		// Preparation-only staging: do not publish SCM ownership or dispatch either shadow shader.
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

namespace
{
	// Engine user data and parent pointers are not reference counted.
	void ReadFullLightOwner(const RE::NiLight* ni, uint32_t& owner, uint32_t& placed) noexcept
	{
		__try {
			const RE::NiAVObject* node = ni;
			for (uint32_t depth = 0; node && depth < 16; ++depth, node = node->parent) {
				if (auto* ref = node->GetUserData()) {
					owner = ref->GetFormID();
					auto* base = ref->GetObjectReference();
					if (base && base->As<RE::TESObjectLIGH>())
						placed = owner;
					return;
				}
			}
		} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
		}
	}
	void ReadFullLightRestPosition(RE::NiLight* ni, uint32_t placedReference, float3& position) noexcept
	{
		__try {
			const auto formID = PointLightFlags::GetRuntimeLightFormID(ni);
			const auto* light = formID ? RE::TESForm::LookupByID<RE::TESObjectLIGH>(formID) : nullptr;
			if (!light && placedReference) {
				auto* reference = RE::TESForm::LookupByID<RE::TESObjectREFR>(placedReference);
				auto* base = reference ? reference->GetObjectReference() : nullptr;
				light = base ? base->As<RE::TESObjectLIGH>() : nullptr;
			}
			const auto& local = ni->local.translate;
			if (light && !light->GetNoFlicker() && ni->parent &&
				ShadowCasterManager::WithinFlickerReach(local.x, local.y, local.z, light->data.flickerMovementAmplitude)) {
				const auto& rest = ni->parent->world.translate;
				if (std::isfinite(rest.x) && std::isfinite(rest.y) && std::isfinite(rest.z))
					position = float3{ rest.x, rest.y, rest.z };
			}
		} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
		}
	}
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
			ReadFullLightOwner(ni, cached.value.referenceID, cached.placedReference);
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
				ReadFullLightRestPosition(ni, cached.placedReference, value.position);
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

void DX12NativeScene::DispatchFullNativeMaterial(bool opaqueReady)
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.WantsMaterialPreparation() ||
		!globals::game::graphicsState || !globals::state || globals::state->isMapMenuOpen ||
		globals::features::dx12Resources.IsLoading() || !nativeSessionStarted ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return;
	CS_GPU_PASS("DX12NativeScene::FullNativeMaterial");
	const auto frame = globals::game::graphicsState->GetFrameCount();
	globals::features::upscaling.nativeComposite.PollFullNative(frame);
	if (!fullNativeMaterial)
		fullNativeMaterial = std::make_unique<DX12Native::NativeScreenProbe>();
	if (fullNativeIndirect)
		fullNativeIndirect->Poll(frame);
	if (fullNativeDirect)
		fullNativeDirect->Poll(frame);
	fullNativeMaterial->Poll(frame);
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
	const auto viewport = Util::ConvertToDynamic(globals::state->screenSize);
	if (!std::isfinite(viewport.x) || !std::isfinite(viewport.y) || viewport.x < 1.0f || viewport.y < 1.0f)
		return;
	if (fullNativeMaterial->WantsFrame(frame, true)) {
		raster.Capture(*backend, Util::GetCurrentSceneDepthSRV(false), &globals::game::frameBufferCached.nonVR, frame);
		fullNativeMaterial->Dispatch(*backend, raster.Get(frame), scene, frame, uint32_t(viewport.x), uint32_t(viewport.y), true, true, false,
			globals::features::extendedMaterials.loaded && globals::features::extendedMaterials.settings.EnableParallax != 0,
			globals::features::extendedMaterials.loaded && globals::features::extendedMaterials.settings.EnableParallaxWarpingFix != 0,
			globals::features::extendedMaterials.loaded && globals::features::extendedMaterials.settings.EnableShadows != 0,
			globals::state->GetMaterialMipBias(Util::GetTemporal()));
	}
	const auto material = fullNativeMaterial->GetMaterialFrame(frame);
	if (!material || material->scene != scene || !scene->inputs || !scene->inputs->residency)
		return;
	const auto& llf = globals::features::lightLimitFix;
	const bool particleSnapshotReady = llf.loaded && llf.nativeParticleFrame == frame &&
		llf.nativeParticlePluginFrame == globals::state->frameCount;
	// Prefer early material dispatch, but defer direct lighting to opaque-ready if
	// the current LLF emitter publication has not arrived yet.
	if (llf.loaded && !particleSnapshotReady && !opaqueReady)
		return;
	if (fullNativeDirect && fullNativeDirect->Get(frame))
		return;
	if (!fullNativeDirect)
		fullNativeDirect = std::make_unique<DX12Native::DirectLightingTrace>();
	const auto prepared = DX12Resources::CollectPreparedLights(*scene->inputs->residency);
	std::vector<DX12Native::DirectPointLight> directLights;
	directLights.reserve(std::min(prepared.size(), size_t(DX12::kMaxPreparedLights)));
	const auto toLinear = [](float value) { return DX12Native::LightMath::ToLinear(value, false); };
	for (const auto& light : prepared) {
		if (directLights.size() >= DX12::kMaxPreparedLights)
			break;
		directLights.push_back({ light.position,
			{ toLinear(light.color.x) * light.fade, toLinear(light.color.y) * light.fade, toLinear(light.color.z) * light.fade },
			light.radius, light.emitterRadius, 0.0f, false });
		// A prepared spotlight without a current projection is not an omni light.
		if (light.flags & (static_cast<uint32_t>(RE::TES_LIGHT_FLAGS::kSpotlight) | static_cast<uint32_t>(RE::TES_LIGHT_FLAGS::kSpotShadow)))
			directLights.back().radius = 0.0f;
	}

	// Full Native samples live values without changing LLF, SCM, or their cached
	// shadow selections. Never call ProcessLight(), which mutates engine data.
	const auto node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
	bool runtimeAvailable = node != nullptr;
	if (node) {
		std::unordered_set<const RE::NiLight*> visited;
		std::vector<bool> matched(directLights.size(), false);
		const size_t preparedCount = directLights.size();
		const auto collect = [&](RE::BSLight* light) {
			if (!light || !light->pointLight || !light->light)
				return;
			auto* ni = light->light.get();
			if (!visited.insert(ni).second || ni->GetFlags().any(RE::NiAVObject::Flag::kHidden))
				return;
			const auto flags = PointLightFlags::GetVanillaPointLightFlags(light, ni);
			const auto runtimeFlags = PointLightFlags::GetRuntimeLightFlags(ni);
			using F = PointLightFlags::Flags;
			if (runtimeFlags & PointLightFlags::ToMask(F::Disabled))
				return;
			const auto& rt = ni->GetLightRuntimeData();
			const auto& position = ni->world.translate;
			if (!std::isfinite(rt.radius.x) || rt.radius.x <= 0.0f ||
				!std::isfinite(rt.fade) || rt.fade <= 0.0f ||
				!std::isfinite(rt.diffuse.red) || !std::isfinite(rt.diffuse.green) || !std::isfinite(rt.diffuse.blue) ||
				!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
				return;
			DX12Native::DirectPointLight value{ { position.x, position.y, position.z },
				{ toLinear(rt.diffuse.red) * rt.fade, toLinear(rt.diffuse.green) * rt.fade, toLinear(rt.diffuse.blue) * rt.fade },
				rt.radius.x, 0.0f, 0.0f, false };
			if (flags & PointLightFlags::ToMask(F::Spot)) {
				auto* spot = skyrim_cast<RE::BSShadowLight*>(light);
				std::array<float, 16> inverse{};
				if (!spot || !spot->GetIsFrustumLight() ||
					!ShadowCasterManager::GetLightProjection(spot, value.worldToSpot.data(), inverse.data()) ||
					!std::ranges::all_of(value.worldToSpot, [](float v) { return std::isfinite(v); }))
					return;
				value.spot = true;
			}
			if (runtimeFlags & PointLightFlags::ToMask(F::InverseSquare)) {
				const auto* ext = LLFCommon::RuntimeLightDataExt::Get(ni);
				if (!std::isfinite(ext->size) || ext->size <= 0.0f || !std::isfinite(ext->cutoffOverride) ||
					ext->cutoffOverride <= 0.0f)
					return;
				value.inverseSquare = true;
				value.radius = ISLMath::CalculateRadius(rt.fade * 4.0f,
					ShadowCasterManager::IsShadowLightType(light), ext->cutoffOverride, ext->size);
				value.fadeZone = 1.0f / std::min(value.radius, ISLMath::FadeZoneBase);
				value.sourceRadius = std::sqrt(ISLMath::ScaledUnitsSq * 0.5f) * ext->size;
				// DirectLighting uses a unitless reciprocal denominator; fold
				// LLF's intensity and unit conversion into the radiance record.
				const float intensity = 4.0f * ISLMath::ScaledUnitsSq;
				value.radiance.x *= intensity;
				value.radiance.y *= intensity;
				value.radiance.z *= intensity;
			}
			uint32_t owner = 0, placed = 0;
			ReadFullLightOwner(ni, owner, placed);
			auto rest = value.position;
			ReadFullLightRestPosition(ni, placed, rest);
			size_t best = preparedCount;
			float bestDistance = std::numeric_limits<float>::infinity();
			bool ambiguous = false;
			for (size_t i = 0; i < preparedCount; ++i) {
				const auto& candidate = prepared[i];
				const float dx = rest.x - candidate.position.x, dy = rest.y - candidate.position.y, dz = rest.z - candidate.position.z;
				const float d2 = dx * dx + dy * dy + dz * dz;
				if (!DX12Native::LightMath::SameEmitterCandidate(candidate.referenceID, placed, d2, candidate.radius))
					continue;
				if (d2 < bestDistance) {
					best = i;
					bestDistance = d2;
					ambiguous = false;
				} else if (d2 == bestDistance) {
					ambiguous = true;
				}
			}
			if (best != preparedCount) {
				if (!ambiguous && !matched[best]) {
					directLights[best] = value;
					matched[best] = true;
				}
				return;  // Never add a second representation of a prepared emitter.
			}
			if (directLights.size() < DX12::kMaxPreparedLights)
				directLights.push_back(value);
		};
		const auto& lights = node->GetRuntimeData();
		for (const auto& light : lights.activeShadowLights)
			collect(light.get());
		for (const auto& light : lights.activeLights)
			collect(light.get());
	}
	// Discard prepared spots for which no live projection was resolved, only
	// after matching so prepared ordinals remain stable during the merge.
	std::erase_if(directLights, [](const auto& light) { return light.radius <= 0.0f; });
	if (particleSnapshotReady) {
		for (const auto& particle : llf.nativeParticleEmitters) {
			if (directLights.size() >= DX12::kMaxPreparedLights)
				break;
			if (!std::isfinite(particle.radius) || particle.radius <= 0.0f || !std::isfinite(particle.fade) || particle.fade <= 0.0f ||
				!std::isfinite(particle.position.x) || !std::isfinite(particle.position.y) || !std::isfinite(particle.position.z) ||
				!std::isfinite(particle.color.x) || !std::isfinite(particle.color.y) || !std::isfinite(particle.color.z))
				continue;
			directLights.push_back({ particle.position,
				{ toLinear(particle.color.x) * particle.fade, toLinear(particle.color.y) * particle.fade,
					toLinear(particle.color.z) * particle.fade }, particle.radius, 0.0f, 0.0f, false });
		}
	}
	DX12Native::DirectDirectionalLight directional;
	// Use the same directional values published by the existing raster
	// constant-buffer producer. Missing/late snapshots never fabricate light.
	float4 sunDirection{}, sunColor{};
	if (globals::state->HasDirectionalShadows() && globals::state->GetNativeDirectionalLight(frame, globals::state->frameCount, sunDirection, sunColor) &&
		std::isfinite(sunDirection.x) && std::isfinite(sunDirection.y) && std::isfinite(sunDirection.z) &&
		std::isfinite(sunColor.x) && std::isfinite(sunColor.y) && std::isfinite(sunColor.z)) {
		const float lengthSquared = sunDirection.x * sunDirection.x + sunDirection.y * sunDirection.y + sunDirection.z * sunDirection.z;
		if (lengthSquared > 1e-8f && std::isfinite(lengthSquared)) {
			const float inverseLength = 1.0f / std::sqrt(lengthSquared);
			directional.direction = { sunDirection.x * inverseLength, sunDirection.y * inverseLength, sunDirection.z * inverseLength, 1.0f };
			directional.radiance = { toLinear(sunColor.x), toLinear(sunColor.y), toLinear(sunColor.z), 0.0f };
		}
	}
	// Selected direct coverage remains partial: sun and particle visibility are
	// limited to the resident TLAS; spots require a valid live engine projection.
	fullNativeDirect->Dispatch(*backend, material, directLights, runtimeAvailable || !prepared.empty() || directional.direction.w != 0.0f,
		directional, uint32_t(viewport.x), uint32_t(viewport.y),
		globals::features::foliageLighting.loaded && globals::features::foliageLighting.GetCommonBufferData().EnableFoliageScattering != 0);
}

void DX12NativeScene::ResetFullNativeEnvironment()
{
	if (fullNativeMaterial)
		globals::features::upscaling.nativeComposite.ResetFullNative();
	if (fullNativeIndirect)
		fullNativeIndirect->Reset();
	if (fullNativeEnvironment)
		fullNativeEnvironment->Reset();
	if (fullNativeEnvironmentStamp.epoch) {
		globals::features::dynamicCubemaps.SetNativeCaptureContext({});
		globals::features::ibl.SetNativeCaptureContext({});
	}
	fullNativeEnvironmentStamp = {};
	++fullNativeEnvironmentEpoch;
}

void DX12NativeScene::DispatchFullNativeIndirect()
{
	using namespace DX12Native;
	using namespace DX12Native::MaterialFrameContract;
	if (!loaded || !globals::features::dx12Control.WantsMaterialPreparation() ||
		!globals::state || !globals::game::graphicsState || globals::state->isMapMenuOpen ||
		globals::features::dx12Resources.IsLoading() || !nativeSessionStarted ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return;
	const auto grid = globals::features::dx12Resources.GetGrid();
	if (!grid || grid->requested.empty() || !grid->requested.front().scope)
		return;
	const auto frame = globals::game::graphicsState->GetFrameCount();
	const auto color = globals::features::linearLighting.GetCommonBufferData();
	const auto encoding = color.enableLinearLighting ?
		(color.enableACEScg ? EnvironmentEncoding::LinearAP1 : EnvironmentEncoding::LinearRec709) :
		EnvironmentEncoding::LegacyScene;
	EnvironmentStamp stamp{ fullNativeEnvironmentEpoch, grid->requested.front().scope, frame,
		globals::state->frameCount, encoding, Util::IsInterior() };
	const float hours = globals::game::calendar ? globals::game::calendar->GetHoursPassed() : fullNativeEnvironmentHours;
	if (fullNativeEnvironmentStamp.epoch && (!fullNativeEnvironmentStamp.SameEnvironment(stamp) ||
		(std::isfinite(hours) && std::abs(hours - fullNativeEnvironmentHours) > 0.01f))) {
		ResetFullNativeEnvironment();
		stamp.epoch = fullNativeEnvironmentEpoch;
	}
	fullNativeEnvironmentStamp = stamp;
	if (std::isfinite(hours)) fullNativeEnvironmentHours = hours;
	auto& cubes = globals::features::dynamicCubemaps;
	auto& ibl = globals::features::ibl;
	cubes.SetNativeCaptureContext(stamp);
	ibl.SetNativeCaptureContext(stamp);
	const auto material = GetFullNativeMaterial(frame);
	if (!backend || !material || material->scope != stamp.scope)
		return;
	if (!fullNativeEnvironment)
		fullNativeEnvironment = std::make_unique<NativeEnvironment>();
	if (!fullNativeIndirect)
		fullNativeIndirect = std::make_unique<NativeIndirectLighting>();
	const auto settings = ibl.GetCommonBufferData();
	const auto utility = globals::features::csUtility.GetCommonBufferData();
	NativeAmbientParameters ambient;
	ambient.dalcValid = globals::state->GetAmbientSH(stamp.engineFrame, stamp.pluginFrame, ambient.dalc);
	ambient.environmentSaturation = settings.EnvIBLSaturation;
	ambient.skySaturation = settings.SkyIBLSaturation;
	ambient.dalcMode = settings.DALCMode;
	ambient.dalcAmount = settings.DALCAmount;
	ambient.iblEnabled = ibl.loaded ? settings.EnableIBL : 0u;
	ambient.ambientMultiplier = utility.ambientLightMult * (color.enableLinearLighting ? color.ambientMult : 1.0f);
	ambient.conversionSaturation = color.conversionSaturation;
	ambient.utilitySaturation = utility.ambientLightColor.saturation;
	ambient.utilityGammaOffset = utility.ambientLightColor.gammaOffset;
	const auto& skylighting = globals::features::skylighting;
	ambient.traceSkyVisibility = skylighting.loaded && skylighting.settings.EnableSkylighting && !stamp.interior;
	ambient.minDiffuseSkyVisibility = std::clamp(skylighting.settings.MinDiffuseVisibility, 0.0f, 1.0f);
	ambient.minSpecularSkyVisibility = std::clamp(skylighting.settings.MinSpecularVisibility, 0.0f, 1.0f);
	ambient.skyMinZ = std::max(0.0f, std::cos(std::clamp(skylighting.settings.MaxZenith, 0.0f, 1.57079632679f)));
	fullNativeEnvironment->Capture(*backend, stamp,
		cubes.loaded ? cubes.GetNativeEnvironment(false) : EnvironmentSource{},
		cubes.loaded && !stamp.interior ? cubes.GetNativeEnvironment(true) : EnvironmentSource{},
		ibl.loaded && !stamp.interior ? ibl.GetNativeSkyRadiance() : EnvironmentSource{});
	fullNativeIndirect->Dispatch(*backend, material, fullNativeEnvironment->Get(),
		uint32_t(IndirectDiffuse) | uint32_t(IndirectSpecular) | uint32_t(Transmission), settings.EnvIBLScale, settings.SkyIBLScale, ambient);
	const auto& draws = fullNativeRasterDraws;
	const bool matchingDraws = draws.finalized && draws.scene == material->scene && draws.stamp.frame == frame;
	auto* tokenSource = matchingDraws && globals::game::renderer ?
		Util::AsReal(globals::game::renderer->GetRuntimeData().renderTargets[MASKS2].texture) : nullptr;
	globals::features::upscaling.nativeComposite.ReconstructFullNative(*backend, material, GetFullNativeDirect(frame), GetFullNativeIndirect(frame),
		globals::state->frameCount, globals::state->ShouldResetHistory(), tokenSource, draws.stamp,
		matchingDraws ? std::span<const RasterPixelIdentity>(draws.tokens) : std::span<const RasterPixelIdentity>{});
}

void DX12NativeScene::OnOpaqueRasterReady()
{
	if (globals::features::dx12Control.WantsMaterialPreparation()) {
		FinishFullNativeRasterDraws();
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

void DX12NativeScene::ObserveFullNativeRasterDraw(RE::BSGeometry& geometry)
{
	using namespace DX12Native::MaterialFrameContract;
	if (!globals::deferred || !globals::deferred->deferredPass || !globals::game::graphicsState ||
		!globals::state || globals::state->isMapMenuOpen || globals::features::dx12Resources.IsLoading() ||
		(globals::state->permutationData.ExtraShaderDescriptor &
			static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return;
	const auto frame = globals::game::graphicsState->GetFrameCount();
	const auto scene = acceleration.GetScene();
	const auto grid = globals::features::dx12Resources.GetGrid();
	if (!scene || !scene->inputs || !scene->materialEvaluation || !scene->shadingGeometry ||
		!grid || grid->requested.empty() || scene->scope != grid->requested.front().scope ||
		scene->geometryRevision != grid->geometryRevision)
		return;
	auto& draws = fullNativeRasterDraws;
	const RasterIdentityStamp stamp{ frame, scene->scope, scene->geometryRevision, scene->materialEvaluation->revision };
	if (draws.stamp.frame != frame || !draws.scene) {
		draws = {};
		draws.scene = scene;
		draws.stamp = stamp;
		draws.seen.resize(scene->inputs->geometry.size());
		draws.tokenBases.resize(scene->inputs->geometry.size());
	}
	// Never merge observations across scene generations in the same frame.
	if (draws.finalized || draws.scene != scene || draws.stamp != stamp)
		return;
	++draws.draws;
	const auto* tri = geometry.AsTriShape();
	const auto& rt = geometry.GetGeometryRuntimeData();
	if (!tri || !rt.rendererData || rt.skinInstance || rt.alphaProperty || geometry.AsDynamicTriShape() ||
		!rt.shaderProperty || rt.shaderProperty->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get()) {
		++draws.rejected;
		return;
	}
	const auto* property = static_cast<const RE::BSLightingShaderProperty*>(rt.shaderProperty.get());
	using PropertyFlag = RE::BSShaderProperty::EShaderPropertyFlag;
	const bool projected = property->flags.any(PropertyFlag::kProjectedUV);
	if (projected) ++draws.projectedDraws;
	const auto counts = tri->GetTrishapeRuntimeData();
	const DX12::CasterSource live{ reinterpret_cast<uintptr_t>(&geometry),
		reinterpret_cast<uintptr_t>(rt.rendererData->rawVertexData), reinterpret_cast<uintptr_t>(rt.rendererData->rawIndexData),
		counts.vertexCount, uint32_t(counts.triangleCount) * 3u, rt.vertexDesc.GetSize(), 0 };
	std::array<float, 12> transform{};
	for (uint32_t row = 0; row < 3; ++row)
		for (uint32_t col = 0; col < 3; ++col)
			transform[4 * row + col] = geometry.world.rotate.entry[row][col] * geometry.world.scale;
	transform[3] = geometry.world.translate.x;
	transform[7] = geometry.world.translate.y;
	transform[11] = geometry.world.translate.z;
	UniqueRasterCandidate match;
	const auto [first, last] = scene->inputs->casterSources.equal_range(live.geometry);
	for (auto it = first; it != last; ++it) {
		const auto ordinal = it->second;
		if (ordinal >= scene->inputs->geometry.size() || ordinal >= UINT32_MAX)
			continue;
		const auto& shape = scene->inputs->geometry[ordinal];
		if (shape && DX12::SameCasterSource(shape->casterSource, live, shape->transform, transform))
			match.Observe(uint32_t(ordinal));
	}
	// Observe projection before base admission: projected receivers are
	// intentionally feature-limited, so they cannot pass exactBaseCandidates.
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
		// Live projection overrides stale preparation classification for admission.
		if (observation.RejectRasterCandidate()) {
			++draws.rejected;
			return;
		}
	} else if (projected) ++draws.unmatchedProjectedDraws;
	const auto& eligible = scene->materialEvaluation->exactBaseCandidates;
	if (!match.Valid() || match.ordinal >= eligible.size() || !eligible[match.ordinal]) {
		if (match.ambiguous)
			++draws.ambiguous;
		else
			++draws.rejected;
		return;
	}
	++draws.candidates;
	if (!draws.seen[match.ordinal]) {
		draws.seen[match.ordinal] = 1;
		++draws.uniqueInstances;
	}
	const auto primitiveCount = uint32_t(counts.triangleCount);
	auto& base = draws.tokenBases[match.ordinal];
	if (!base) {
		const auto next = RasterTokenBase(uint32_t(draws.tokens.size()), primitiveCount);
		if (!next) {
			++draws.tokenBudgetRejected;
			return;
		}
		for (uint32_t primitive = 0; primitive < primitiveCount; ++primitive)
			draws.tokens.push_back({ match.ordinal, primitive });
		base = next;
	}
	globals::state->permutationData.nativeRasterTokenBase = base;
	globals::state->permutationData.nativeRasterPrimitiveCount = primitiveCount;
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
	for (size_t i = 0; i < slots.size(); ++i) {
		auto* view = Util::AsReal(state.PSTexture[slots[i]]);
		if (view) {
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			view->GetDesc(&desc);
			if (desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || desc.Format == DXGI_FORMAT_UNKNOWN)
				return;
			views[i].copy_from(view);
		}
	}
	// Noise is required even for the solid-color projection permutation.
	if (!views.back())
		return;
	auto& observation = draws.projectionTextures[ordinal];
	if (observation.conflicted)
		return;
	if (observation.observed) {
		bool same = observation.descriptor == descriptor;
		for (size_t i = 0; i < views.size(); ++i)
			same = same && observation.views[i].get() == views[i].get();
		if (!same) {
			observation.conflicted = true;
			observation.views = {};
		}
		return;
	}
	observation.descriptor = descriptor;
	observation.views = std::move(views);
	observation.observed = true;
}

void DX12NativeScene::FinishFullNativeRasterDraws()
{
	if (!globals::game::graphicsState)
		return;
	auto& draws = fullNativeRasterDraws;
	if (draws.stamp.frame != globals::game::graphicsState->GetFrameCount() ||
		draws.scene != acceleration.GetScene()) {
		draws = {};
		return;
	}
	// A later projected/conflicting pass invalidates tokens already emitted
	// by an earlier pass for this instance. Preserve token ordinals.
	for (auto& token : draws.tokens) {
		const auto observation = draws.projection.find(token.instance);
		if (observation != draws.projection.end() && observation->second.RejectRasterCandidate())
			token = {};
	}
	draws.finalized = true;
}

std::function<void()> DX12NativeScene::OnRenderPassBegin(const RE::BSRenderPass* pass)
{
	if (loaded && !globals::game::isVR && globals::features::dx12Control.WantsMaterialPreparation()) {
		if (!globals::state)
			return nullptr;
		auto& data = globals::state->permutationData;
		const auto oldBase = data.nativeRasterTokenBase;
		const auto oldCount = data.nativeRasterPrimitiveCount;
		const auto oldProjection = fullNativeRasterDraws.activeProjectedInstance;
		const auto oldStamp = fullNativeRasterDraws.stamp;
		fullNativeRasterDraws.activeProjectedInstance = UINT32_MAX;
		data.nativeRasterTokenBase = data.nativeRasterPrimitiveCount = 0;
		if (pass && pass->geometry)
			ObserveFullNativeRasterDraw(*pass->geometry);
		return [this, oldBase, oldCount, oldProjection, oldStamp] {
			fullNativeRasterDraws.activeProjectedInstance =
				fullNativeRasterDraws.stamp == oldStamp && !fullNativeRasterDraws.finalized ? oldProjection : UINT32_MAX;
			if (globals::state) {
				globals::state->permutationData.nativeRasterTokenBase = oldBase;
				globals::state->permutationData.nativeRasterPrimitiveCount = oldCount;
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
	const RE::TESForm* reference = nullptr;
	RE::NiAVObject* node = &geometry;
	for (uint32_t depth = 0; node && depth < 16; ++depth, node = node->parent) {
		if (const auto* owner = node->GetUserData()) {
			reference = owner;
			break;
		}
	}
	if (reference && (reference->IsDynamicForm() || reference->IsActor()))
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
		ResetFullNativeEnvironment();
		if (fullNativeDirect)
			fullNativeDirect->Reset();
		if (fullNativeMaterial)
			fullNativeMaterial->Reset();
		raster.Reset();
		shadows.Reset();
		acceleration.Reset();
		geometryCapture.Reset();
	}
}

void DX12NativeScene::OnRuntimeDisabled()
{
	loadingResetPending.store(false, std::memory_order_release);
	frameNativeModeID.store(UINT32_MAX, std::memory_order_release);
	frameNativeMode.store(DX12::SCMNativeMode::Disabled, std::memory_order_release);
	frameExactShadowLights.store(false, std::memory_order_relaxed);
	movingOriginCount.store(0, std::memory_order_relaxed);
	if (!nativeSessionStarted)
		return;
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
	fullNativeIndirect.reset();
	fullNativeEnvironment.reset();
	fullNativeDirect.reset();
	fullNativeMaterial.reset();
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
	const auto [first, last] = scene->inputs->casterSources.equal_range(live.geometry);
	for (auto it = first; it != last; ++it) {
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
		uint32_t selectedTextureInstances = 0, completeTextureViews = 0, conflictingTextures = 0;
		for (const auto& [ordinal, observation] : draws.projectionTextures) {
			const auto property = draws.projection.find(ordinal);
			if (property == draws.projection.end() || !property->second.ValidProjection())
				continue;
			if (observation.conflicted) ++conflictingTextures;
			else if (observation.observed) {
				++selectedTextureInstances;
				if (std::ranges::all_of(observation.views, [](const auto& view) { return bool(view); }))
					++completeTextureViews;
			}
		}
		status["fullNativeProjectionInputs"] = { { "frame", draws.stamp.frame }, { "finalized", draws.finalized },
			{ "projectedDraws", draws.projectedDraws }, { "unmatchedDraws", draws.unmatchedProjectedDraws },
			{ "validPropertyInstances", validProjection }, { "conflictingInstances", conflictingProjection },
			{ "selectedTextureInstances", selectedTextureInstances }, { "completeTextureViews", completeTextureViews },
			{ "conflictingTextureInstances", conflictingTextures },
			{ "projectorConstantsReady", false }, { "projectedTexturesReady", false } };
		status["fullNativeRasterAdmission"] = { { "frame", draws.stamp.frame }, { "scope", draws.stamp.scope },
			{ "geometryRevision", draws.stamp.geometryRevision }, { "materialRevision", draws.stamp.materialRevision },
			{ "draws", draws.draws }, { "candidateDraws", draws.candidates }, { "uniqueInstances", draws.uniqueInstances },
			{ "ambiguousDraws", draws.ambiguous }, { "rejectedDraws", draws.rejected }, { "finalized", draws.finalized },
			{ "primitiveTokens", draws.tokens.size() }, { "tokenBudgetRejected", draws.tokenBudgetRejected },
			{ "exactPixelIdentity", false }, { "rasterReplacement", false } };
		const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : UINT32_MAX;
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

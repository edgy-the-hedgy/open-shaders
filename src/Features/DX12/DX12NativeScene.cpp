#include "DX12NativeScene.h"

#include "../Upscaling.h"
#include "../ReverseZ.h"
#include "../LinearLighting.h"
#include "DX12Control.h"
#include "DX12Resources.h"
#include "Globals.h"
#include "Materials/SharedMaterialHooks.h"
#include "GpuPass.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/N/NiAlphaProperty.h"
#include "State.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <ranges>
#include <unordered_set>

void DX12NativeScene::DrawSettings()
{
	ImGui::Checkbox(T("menu.dx12.scene.independent_shadowmask", "Independent shadowmask"), &shadowMaskEnabled);
	ImGui::Checkbox(T("menu.dx12.scene.outdoor_shadow_offloading", "Enable outdoor shadow offloading (DX12 only)"), &outdoorShadowOffloading);
	bool keepRasterStatic = !offloadStaticShadows;
	if (ImGui::Checkbox(T("menu.dx12.scene.keep_raster_static", "Keep raster static shadows for DX12-accounted lights"), &keepRasterStatic))
		offloadStaticShadows = !keepRasterStatic;
	bool retainFlickeringRaster = !offloadFlickeringStatic;
	if (ImGui::Checkbox(T("menu.dx12.scene.keep_raster_flickering", "Keep raster static shadows for flickering lights"), &retainFlickeringRaster))
		offloadFlickeringStatic = !retainFlickeringRaster;
	if (ImGui::Checkbox(T("menu.dx12.scene.freeze_flickering_native", "Freeze flickering shadows (DX12 only)"), &freezeNativeFlickering)) {
		lighting.ClearFrozenLights();
	}
	const char* fractions[] = {
		T("menu.dx12.scene.density.full", "Full"), T("menu.dx12.scene.density.two_thirds", "Two-thirds"), T("menu.dx12.scene.density.half", "Half"), T("menu.dx12.scene.density.one_third", "One-third"), T("menu.dx12.scene.density.quarter", "Quarter"),
		T("menu.dx12.scene.density.one_sixth", "One-sixth"), T("menu.dx12.scene.density.one_ninth", "One-ninth"), T("menu.dx12.scene.density.four_ninths", "Four-ninths")
	};
	int mode = static_cast<int>(densityMode);
	if (ImGui::Combo(T("menu.dx12.scene.trace_density", "Trace density"), &mode, fractions, 8))
		densityMode = static_cast<uint32_t>(mode);
	ImGui::Checkbox(T("menu.dx12.scene.exclude_near_light", "Exclude shadows near light geometry"), &nearLightShadowExclusion);
	ImGui::BeginDisabled(!nearLightShadowExclusion);
	int nearUnits = int(nearLightExclusionUnits);
	if (ImGui::SliderInt(T("menu.dx12.scene.exclusion_radius_units", "Light-centered exclusion radius (units)"), &nearUnits, 0, 512))
		nearLightExclusionUnits = uint32_t(std::clamp(((nearUnits + 2) / 4) * 4, 0, 512));
	ImGui::EndDisabled();
	ImGui::SliderFloat(T("menu.dx12.scene.shadow_reach_multiplier", "Shadow reach multiplier (1x = vanilla)"), &shadowReachMultiplier,
		0.1f, 64.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
	float fade = shadowFadeRange;
	if (ImGui::SliderFloat(T("menu.dx12.scene.shadow_fade_fraction", "Shadow fade range fraction"), &fade, 0.0f, 1.0f, "%.2f"))
		shadowFadeRange = std::round(fade * 50.0f) / 50.0f;
	ImGui::SliderFloat(T("menu.dx12.scene.shadow_falloff_curve", "Shadow falloff curve"), &shadowFalloffCurve, 0.0f, 2.5f, "%.2f");
}

void DX12NativeScene::DrawLightingSettings(bool /*developer*/)
{
	const bool exclusive = globals::features::dx12Control.UsesExclusiveMaterials();
	ImGui::BeginDisabled(!exclusive);
	ImGui::Checkbox(T("menu.dx12.lighting.direct", "Direct lighting"), &directLightingEnabled);
	if (directLightingEnabled) {
		const char* views[] = {
			T("menu.dx12.lighting.view_raster", "Raster composite (preserve illumination)"),
			T("menu.dx12.lighting.view_diffuse", "Direct diffuse"),
			T("menu.dx12.lighting.view_specular", "Direct specular"),
			T("menu.dx12.lighting.view_combined", "Combined direct"),
			T("menu.dx12.lighting.view_reach", "Light influence"),
			T("menu.dx12.lighting.view_visibility", "Ray visibility"),
			T("menu.dx12.lighting.view_coverage", "Lighting coverage"),
			T("menu.dx12.lighting.view_normals", "Surface normals") };
		ImGui::Combo(T("menu.dx12.lighting.view", "Lighting output"), &lightingView, views, int(std::size(views)));
	}
	ImGui::EndDisabled();
	ImGui::TextWrapped("%s", exclusive ?
		T("menu.dx12.lighting.native_direct_info", "Direct signals are evaluated independently. Raster composite preserves ambient and indirect light; lighting outputs are previews only.") :
		T("menu.dx12.lighting.raster_active", "Raster lighting remains active."));
}

void DX12NativeScene::DrawMaterialSettings(bool developer)
{
	ImGui::TextWrapped("%s", globals::features::dx12Control.UsesExclusiveMaterials() ?
		T("menu.dx12.materials.native_selected", "Full-resolution material evaluation is active. Uncovered surfaces may render incorrectly.") :
		T("menu.dx12.materials.raster_active", "Raster material rendering remains active. DX12 material evaluation is experimental."));
	ImGui::Checkbox(T("menu.dx12.materials.coverage", "Material coverage visualization"), &materialCoveragePreview);
	if (materialCoveragePreview)
		ImGui::TextWrapped("%s", T("menu.dx12.materials.coverage_legend", "Green: evaluated | Cyan: base-only | Red: missing material | Amber: no hit | Purple: depth mismatch | Gray: invalid depth"));
	if (!developer)
		return;
	if (ImGui::Checkbox(T("menu.dx12.materials.probe", "Visible-pixel material probe"), &screenProbeEnabled) && !screenProbeEnabled)
		screenProbe.Reset();
	if (screenProbeEnabled && ImGui::Checkbox(T("menu.dx12.materials.full_resolution", "Full render resolution"), &fullResolutionMaterialProbe))
		screenProbe.Reset();
	if (screenProbeEnabled && ImGui::Checkbox(T("menu.dx12.materials.continuous_probe", "Continuous material probe (when GPU slot is free)"), &continuousMaterialProbe))
		screenProbe.Reset();
}

void DX12NativeScene::DrawDevStats()
{
	ImGui::SeparatorText(T("menu.dx12.dev.scene_materials", "Scene / material preparation"));
	if (globals::features::dx12Control.IsFullNativeMode()) {
		const auto& stats = screenProbe.stats;
		if (stats.sampled) ImGui::Text("Material output: %u x %u | %.2f MiB GPU | 32-byte counter readback", stats.width, stats.height, double(stats.outputBytes)/1048576.0);
		if (screenProbe.submittedFrames) ImGui::Text("Material submissions: %u | Reused output allocations: %u", screenProbe.submittedFrames, screenProbe.reusedAllocations);
		if (stats.sampled) ImGui::Text("Screen frame %u | Evaluated %u / %u | No hit %u | Depth mismatch %u | Evaluation failed %u | Invalid depth %u",
			stats.frame, stats.evaluated + stats.baseOnly, stats.sampled, stats.noHit, stats.depthMismatch, stats.missingMaterial, stats.invalid);
		if (stats.sampled) ImGui::Text("Base-only %u | Evaluated base %u | Unsupported %u | Missing textures %u", stats.baseOnly, stats.evaluated, stats.unsupportedMaterial, stats.missingTextures);
		if ((screenProbeEnabled || materialCoveragePreview || globals::features::dx12Control.UsesExclusiveMaterials()) && !screenProbe.failure.empty()) ImGui::TextWrapped("%s", screenProbe.failure.c_str());
		if (directLightingEnabled) {
			const auto& lightStats = lighting.DirectStats();
			const auto& selection = lighting.GetSelectionStats();
			ImGui::Text("Prepared: %u | Runtime: %u%s | Matched: %u | Supplemented: %u | Overflow: %u",
				selection.prepared, selection.observed, selection.runtimeAvailable ? "" : " (unavailable)",
				selection.matched, selection.supplemented, selection.overflow);
			ImGui::Text("Direct lights dispatched: %u | GPU frame: %u", lighting.DirectLightCount(), lightStats.frame);
			ImGui::Text("Lighting valid: %u | No influence: %u | Occluded: %u | Visible: %u",
				lightStats.counts[0], lightStats.counts[1], lightStats.counts[2], lightStats.counts[3]);
			ImGui::Text("Tile candidates: %u | Tiles: %u | Visibility rays: %llu", lightStats.counts[4], lightStats.counts[5],
				static_cast<unsigned long long>((uint64_t(lightStats.counts[7]) << 32) | lightStats.counts[6]));
		}
		if (directLightingEnabled && !lighting.DirectFailure().empty()) ImGui::TextWrapped("%s", lighting.DirectFailure().c_str());
	}
	const auto built = acceleration.GetScene();
	if (!built || !built->inputs) {
		ImGui::TextDisabled("%s", T("menu.dx12.dev.no_scene", "No completed scene"));
		return;
	}
	std::unordered_set<const DX12Native::BottomLevel*> unique;
	for (const auto& bottom : built->bottoms)
		if (bottom) unique.insert(bottom.get());
	ImGui::Text("TLAS instances: %zu | Unique BLAS: %zu", built->inputs->geometry.size(), unique.size());
	if (const auto& materials = built->preparedMaterials) {
		ImGui::Text("Preliminary recipes: %zu | Missing sources: %u | Unsupported: %u", materials->recipes.size(), materials->missing, materials->unsupported);
		ImGui::Text("Missing: runtime %u | Companion %u | Runtime material sources: %u",
			materials->runtimeMissing, materials->companionMissing, materials->runtimeSources);
		ImGui::Text("Companion materials recovered from matching live geometry: %u", materials->recoveredPlacements);
		ImGui::Text("Alpha-tested material sources: %u | TXST: %u | MATO: %u", materials->alphaTested, materials->resolvedTextureSets, materials->resolvedMaterialObjects);
		if (ImGui::TreeNode("Missing runtime material reasons")) {
			const char* names[] = {"No observation", "Missing shader material", "Missing vertex inputs", "Non-PBR marker", "Unsupported shader flags", "Opacity or clamp", "Missing texture set", "Invalid UV", "Invalid texture path", "Missing required textures", "Invalid vertex attributes"};
			static_assert(std::size(names) == size_t(DX12::RuntimeMaterialFailure::Count));
			for (size_t i=0; i<std::size(names); ++i)
				if (materials->runtimeFailures[i]) ImGui::Text("%s: %u", names[i], materials->runtimeFailures[i]);
			ImGui::TreePop();
		}
		ImGui::Text("Unsupported (overlapping): ambiguous %u | non-PBR %u | alpha %u | basis %u | UV/clamp %u",
			materials->rejected[0], materials->rejected[1], materials->rejected[2], materials->rejected[3], materials->rejected[4]);
		ImGui::TextDisabled("%s", globals::features::dx12Control.UsesExclusiveMaterials() ?
			"Exclusive DX12 material authority selected" : "Diagnostic material evaluation only; raster rendering authority unchanged");
		if (const auto& shading = built->shadingGeometry)
			ImGui::Text("GPU shading geometry: %u / %u instances | %.2f MiB", shading->readyInstances, shading->instanceCount, double(shading->bytes) / 1048576.0);
		else if (!built->shadingFailure.empty())
			ImGui::TextWrapped("%s", built->shadingFailure.c_str());
		if (const auto& evaluation = built->materialEvaluation) {
			ImGui::Text("Resident recipes: %u | DDS: %zu (archive %u) | Reused: %u | Failed: %u", evaluation->residentRecipes,
				evaluation->textures.size(), evaluation->archiveTextures, evaluation->reusedTextures, evaluation->failedTextures);
			ImGui::Text("Base-only material bindings: %u", evaluation->baseOnlyBindings);
			ImGui::Text("Specialized DDS roles: %u / %u | Shared references: %u | Texture slots: %zu / %u",
				evaluation->specializedResident, evaluation->specializedRequested, evaluation->sharedTextureReferences,
				evaluation->poolTextures.size(), NativePBR::kGpuTexturePoolCapacity);
			const auto legacy = SharedMaterialHooks::LegacyResidency();
			ImGui::Text("Legacy PBR material extensions: %zu mesh / %zu landscape | Glints texture: %s", legacy.meshes, legacy.landscapes, legacy.glints ? "resident" : "none");
			if (ImGui::TreeNode("DDS failure details")) {
				for (const auto& [reason, count] : evaluation->failureReasons) ImGui::Text("%s: %u", reason.c_str(), count);
				for (const auto& [path, reason] : evaluation->failedExamples) ImGui::TextWrapped("%s: %s", reason.c_str(), path.c_str());
				ImGui::TreePop();
			}
			ImGui::Text(T("menu.dx12.dev.dds_vram", "DDS VRAM: %.2f MiB | Evaluated triangles: %u / %u"), double(evaluation->textureBytes) / 1048576.0,
				evaluation->evaluated, evaluation->instanceCount);
			if (evaluation->evaluated) {
				const auto& sample = evaluation->firstSample;
				ImGui::Text("Sample base RGB: %.3f %.3f %.3f | Roughness %.3f | Metal %.3f | AO %.3f",
					sample.color[0], sample.color[1], sample.color[2], sample.normal[3], sample.f0[3], sample.emission[3]);
			}
		} else if (!built->materialFailure.empty())
			ImGui::TextWrapped("%s", built->materialFailure.c_str());
	}
}

void DX12NativeScene::LoadSettings(json& input)
{
	directLightingEnabled = input.value("DirectLighting", false);
	lightingView = input.value("LightingView", 0);
	if (lightingView < 0 || lightingView > 7) lightingView = 0;
	shadowMaskEnabled = input.value("ShadowMask", true);
	outdoorShadowOffloading = input.value("OutdoorShadowOffloading", false);
	offloadStaticShadows = input.value("OffloadSCMStatic", true);
	offloadFlickeringStatic = input.value("OffloadFlickeringStatic", true);
	densityMode = std::min(input.value("ShadowTraceDensityMode", 7u), 7u);
	freezeNativeFlickering = input.value("FreezeNativeFlickering", false);
	{
		lighting.ClearFrozenLights();
	}
	nearLightShadowExclusion = input.value("ShadowNearLightExclusion", true);
	nearLightExclusionUnits = std::clamp(input.value("ShadowNearLightRadius", 16u), 0u, 512u) / 4u * 4u;
	shadowReachMultiplier = std::clamp(input.value("ShadowReachMultiplier", 1.0f), 0.1f, 64.0f);
	shadowFadeRange = std::round(std::clamp(input.value("ShadowFadeRange", 0.66f), 0.0f, 1.0f) * 50.0f) / 50.0f;
	shadowFalloffCurve = std::clamp(input.value("ShadowFalloffCurve", 1.0f), 0.0f, 2.5f);
}

void DX12NativeScene::SaveSettings(json& output)
{
	output = { { "DirectLighting", directLightingEnabled }, { "LightingView", lightingView }, { "ShadowMask", shadowMaskEnabled }, { "OutdoorShadowOffloading", outdoorShadowOffloading }, { "ShadowTraceDensityMode", densityMode },
		{ "OffloadSCMStatic", offloadStaticShadows },
		{ "OffloadFlickeringStatic", offloadFlickeringStatic },
		{ "FreezeNativeFlickering", freezeNativeFlickering },
		{ "ShadowNearLightExclusion", nearLightShadowExclusion },
		{ "ShadowNearLightRadius", nearLightExclusionUnits },
		{ "ShadowReachMultiplier", shadowReachMultiplier },
		{ "ShadowFadeRange", shadowFadeRange },
		{ "ShadowFalloffCurve", shadowFalloffCurve } };
}

void DX12NativeScene::RestoreDefaultSettings()
{
	directLightingEnabled = false;
	lightingView = 0;
	materialCoveragePreview = false;
	shadowMaskEnabled = true;
	outdoorShadowOffloading = false;
	offloadStaticShadows = true;
	offloadFlickeringStatic = true;
	densityMode = 7;
	freezeNativeFlickering = false;
	{
		lighting.ClearFrozenLights();
	}
	nearLightShadowExclusion = true;
	nearLightExclusionUnits = 16;
	shadowReachMultiplier = 1.0f;
	shadowFadeRange = 0.66f;
	shadowFalloffCurve = 1.0f;
}

bool DX12NativeScene::CanOffloadShadowsInCurrentCell() const
{
	const auto* player = globals::game::player;
	const auto* cell = player ? player->GetParentCell() : nullptr;
	return cell && (!cell->IsExteriorCell() || outdoorShadowOffloading);
}

bool DX12NativeScene::OwnsStaticForLight(RE::BSShadowLight* light, int32_t slot) const
{
	// SCM keeps raster dynamic casters unconditionally. Static casters may be
	// omitted only when this exact runtime light was matched to a native emitter
	// in the current frame; a nearby prepared light is not sufficient proof.
	if (!light || slot < 0 || !loaded || globals::game::isVR ||
		!globals::features::dx12Control.IsActive() ||
		!(globals::features::dx12Control.IsShadowOffloadMode() || globals::features::dx12Control.UsesExclusiveMaterials()) ||
		!shadowMaskEnabled || !offloadStaticShadows || !CanOffloadShadowsInCurrentCell() ||
		light->GetIsFrustumOrDirectionalLight() || !globals::game::graphicsState)
		return false;
	const auto scene = acceleration.GetScene();
	const auto* ni = light->light.get();
	if (!scene || !scene->acceleration || !ni)
		return false;

	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	return lighting.OwnsStaticForLight(ni, frame, offloadFlickeringStatic);
}

void DX12NativeScene::ObserveRuntimeLight(RE::BSShadowLight* light, uint32_t referenceID,
	bool flickers, const RE::NiPoint3& restOrigin)
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive() ||
		!light || light->GetIsFrustumOrDirectionalLight() || !globals::game::graphicsState)
		return;
	lighting.ObserveRuntimeLight(light, referenceID, flickers, restOrigin,
		globals::game::graphicsState->GetFrameCount(), freezeNativeFlickering);
}

void DX12NativeScene::PreShadowMaps()
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive()) {
		lighting.PublishPreparedLights(nullptr);
		return;
	}
	auto grid = globals::features::dx12Resources.GetGrid();
	if (!grid || grid->requested.empty() || !grid->requested.front().scope) {
		lighting.PublishPreparedLights(nullptr);
		return;
	}
	// SCM reads a single per-frame publication without re-enumerating tiles
	// for every shadow light; the same ordered sources feed RayQuery.
	lighting.PublishPreparedLights(std::make_shared<const std::vector<DX12::PreparedLight>>(
		globals::features::dx12Resources.GetPreparedLights()));
	// Only a revision-coalesced mailbox handoff on this thread. All BLAS/TLAS
	// allocations, command recording and GPU builds run on their own worker.
	if (acceleration.Start(globals::d3d::device, globals::d3d::context))
		acceleration.Request(std::move(grid));
}

void DX12NativeScene::OnOpaqueRasterReady(const OpaqueRasterInputs& /*a_inputs*/)
{
	CS_GPU_PASS("DX12NativeScene::OpaqueRasterReady");
	opaqueLighting.reset();
	opaqueRaster.reset();
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive())
		return;
	if (!globals::game::graphicsState || !globals::state || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor &
			static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return;
	screenProbe.Poll();
	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	lighting.PollDirect(frame);
	const auto& control = globals::features::dx12Control;
	const bool fullNative = control.IsFullNativeMode();
	const bool exclusiveMaterials = control.UsesExclusiveMaterials();
	const bool directEnabled = fullNative && exclusiveMaterials && directLightingEnabled;
	const bool materialRequired = exclusiveMaterials || materialCoveragePreview;
	const bool fullProbe = fullNative && (screenProbeEnabled || materialRequired) &&
		screenProbe.WantsFrame(frame, continuousMaterialProbe || materialRequired);
	const bool shadowTrace = shadowMaskEnabled && (control.IsShadowOffloadMode() || (fullNative && exclusiveMaterials)) && CanOffloadShadowsInCurrentCell();
	if (!fullProbe && !shadowTrace) return;

	if (!backend) {
		std::string reason;
		backend = GraphicsBackend::Create(globals::d3d::device, globals::d3d::context, reason);
		if (!backend) return;
	}

	const auto viewport = Util::ConvertToDynamic(globals::state->screenSize);
	const uint32_t viewportWidth = std::isfinite(viewport.x) && viewport.x > 0.0f ? uint32_t(viewport.x) : 0u;
	const uint32_t viewportHeight = std::isfinite(viewport.y) && viewport.y > 0.0f ? uint32_t(viewport.y) : 0u;
	auto built = acceleration.GetScene();
	if (fullNative) {
		const auto grid = globals::features::dx12Resources.GetGrid();
		if (!grid || grid->requested.empty() || !built || built->scope != grid->requested.front().scope)
			built.reset();
	}
	using DX12Native::MaterialFrameContract::TemporalIdentity;
	TemporalIdentity temporal{ frame, globals::state->frameCount, viewportWidth, viewportHeight,
		built ? built->scope : 0u, historyRevision, globals::features::reverseZ.IsActive(), false };
	if (globals::state->ShouldResetHistory() || !temporal.CanReuseHistory(lastTemporal)) {
		temporal.historyRevision = ++historyRevision;
		temporal.resetHistory = true;
	}
	lastTemporal = temporal;
	raster.Capture(*backend, Util::GetCurrentSceneDepthSRV(false),
		&globals::game::frameBufferCached.nonVR, temporal);
	const auto captured = raster.Get(frame);
	opaqueRaster = captured;
	const DX12Native::ShadowTraceControls controls{
		.viewportWidth = viewportWidth,
		.viewportHeight = viewportHeight,
		.densityMode = densityMode,
		.nearLightExclusion = nearLightShadowExclusion,
		.nearLightRadius = float(nearLightExclusionUnits),
		.reachMultiplier = shadowReachMultiplier,
		.fadeRange = shadowFadeRange,
		.falloffCurve = shadowFalloffCurve
	};
	if (fullProbe)
		screenProbe.Dispatch(*backend, captured, built, frame, viewportWidth, viewportHeight,
			fullResolutionMaterialProbe || materialRequired, continuousMaterialProbe || materialRequired, screenProbeEnabled || materialCoveragePreview);
	if (directEnabled)
		lighting.DispatchDirect(*backend, screenProbe.GetMaterialFrame(frame), controls);
	if (shadowTrace)
		lighting.DispatchShadows(*backend, captured, built, frame, controls);
}

bool DX12NativeScene::PublishOpaqueLighting(std::shared_ptr<const DX12Native::OpaqueLightingFrame> frame)
{
	if (!frame || !opaqueRaster || !globals::features::dx12Control.UsesExclusiveMaterials() ||
		shadowMaskEnabled || frame->material != screenProbe.GetMaterialFrame(opaqueRaster->frame) ||
		!frame->material || frame->material->raster != opaqueRaster || !frame->IsCoherent(frame->material))
		return false;
	opaqueLighting = std::move(frame);
	return true;
}

void DX12NativeScene::OnOpaqueCompositeReady(const OpaqueRasterInputs& inputs)
{
	CS_GPU_PASS("DX12NativeScene::OpaqueCompositeReady");
	auto complete = std::exchange(opaqueLighting, {});
	auto captured = std::exchange(opaqueRaster, {});
	if (!complete || !captured || !backend || !inputs.litColor || !globals::state ||
		!globals::game::graphicsState || globals::game::isVR || shadowMaskEnabled ||
		!globals::features::dx12Control.UsesExclusiveMaterials() ||
		complete->material->raster != captured ||
		complete->temporal.nativeFrame != globals::game::graphicsState->GetFrameCount() ||
		complete->temporal.pluginFrame != globals::state->frameCount)
		return;
	const auto transfer = globals::features::linearLighting.IsLinearLightingActive() ?
		DX12Native::NativeComposite::Transfer::Linear : DX12Native::NativeComposite::Transfer::LegacyScene;
	(void)opaqueComposite.Capture(backend->d3d11Device.get(), backend->d3d11Context.get(),
		backend->d3d12Device.get(), backend->commandQueue.get(), inputs.litColor,
		complete->temporal.pluginFrame, complete->temporal.nativeFrame, complete->temporal.width,
		complete->temporal.height, transfer, {}, true, false, complete->material, {}, 0, complete);
}

std::function<void()> DX12NativeScene::OnRenderPassBegin(const RE::BSRenderPass* pass)
{
	// Observe visible static geometry already drawn by the existing opaque
	// render path. This does not change render-pass state or DX11 draw behavior.
	const auto& control = globals::features::dx12Control;
	const bool fullNative = control.IsFullNativeMode();
	if (!pass || !pass->geometry || !loaded || globals::game::isVR ||
		!control.IsActive() ||
		(!fullNative && (!shadowMaskEnabled || !control.IsShadowOffloadMode() ||
			!globals::features::upscaling.loaded)) ||
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
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive())
		return;
	auto* tri = geometry.AsTriShape();
	if (!tri || geometry.AsDynamicTriShape())
		return;
	const auto& runtime = geometry.GetGeometryRuntimeData();
	if (runtime.skinInstance || !runtime.rendererData ||
		!runtime.rendererData->rawVertexData || !runtime.rendererData->rawIndexData)
		return;
	const bool fullNative = globals::features::dx12Control.IsFullNativeMode();
	if (fullNative && globals::state && globals::state->isMapMenuOpen) return;
	const auto* alpha = runtime.alphaProperty.get();
	// Only the full-native material query understands non-opaque candidates.
	// Blending, animated opacity and unsupported compare operations remain excluded.
	if (alpha && (!fullNative || alpha->GetAlphaBlending() || !alpha->GetAlphaTesting() ||
		((alpha->alphaFlags >> 10) & 7u) != 4u && ((alpha->alphaFlags >> 10) & 7u) != 6u)) return;
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
	if (fullNative) {
		candidate.fullNative = true;
		candidate.alphaTest = alpha != nullptr;
		if (alpha) { candidate.alphaThreshold = alpha->alphaThreshold; candidate.alphaFunction = uint8_t((alpha->alphaFlags >> 10) & 7u); }
		using V = RE::BSGraphics::Vertex;
		candidate.shadingProperty = static_cast<const RE::BSLightingShaderProperty*>(property);
		if (reference)
			candidate.materialObjectEditorID = SharedMaterialHooks::ResolveMaterialObjectEditorID(reference->As<RE::TESObjectREFR>());
		if (desc.HasFlag(V::VF_UV) && desc.HasFlag(V::VF_NORMAL)) {
			candidate.uvOffset = desc.GetAttributeOffset(V::VA_TEXCOORD0);
			candidate.normalOffset = desc.GetAttributeOffset(V::VA_NORMAL);
			if (desc.HasFlag(V::VF_TANGENT)) candidate.tangentOffset = desc.GetAttributeOffset(V::VA_BINORMAL);
			if (desc.HasFlag(V::VF_COLORS)) candidate.colorOffset = desc.GetAttributeOffset(V::VA_COLOR);
		}
	}
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

void DX12NativeScene::OnSceneTransitionReset(bool a_opening)
{
	if (a_opening) {
		++historyRevision;
		lastTemporal = {};
		opaqueRaster.reset();
		opaqueLighting.reset();
		lighting.Reset();
		raster.Reset();
		screenProbe.Reset();
		acceleration.Reset();
		geometryCapture.Reset();
	}
}

void DX12NativeScene::OnRuntimeDisabled()
{
	++historyRevision;
	lastTemporal = {};
	opaqueRaster.reset();
	opaqueLighting.reset();
	lighting.Reset();
	raster.Reset();
	screenProbe.Reset();
	acceleration.Reset();
	geometryCapture.Reset();
}

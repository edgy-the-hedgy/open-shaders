#include "DX12NativeScene.h"

#include "DX12Control.h"
#include "DX12Resources.h"
#include "RuntimeResidencyPolicy.h"
#include "Globals.h"
#include "GpuPass.h"
#include "RE/T/TESObjectCELL.h"
#include "State.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <ranges>

void DX12NativeScene::DrawSettings()
{
	ImGui::Checkbox(T("menu.dx12.scene.independent_shadowmask", "Independent shadowmask"), &shadowMaskEnabled);
	ImGui::Checkbox(T("menu.dx12.scene.outdoor_shadow_offloading", "Enable outdoor shadow offloading (DX12 only)"), &outdoorShadowOffloading);
	bool preview = previewRuntimeShadows.load(std::memory_order_relaxed);
	if (ImGui::Checkbox(T("menu.dx12.runtime.preview", "Diagnostic shadow overlay (raster retained)"), &preview))
		previewRuntimeShadows.store(preview, std::memory_order_relaxed);
	ImGui::TextDisabled("%s", T("menu.dx12.runtime.preview_warning", "Preview may double-darken shadows; raster ownership is unchanged."));
	if (ImGui::Checkbox(T("menu.dx12.scene.freeze_flickering_native", "Freeze flickering native shadows (DX12 only)"), &freezeNativeFlickering)) {
		std::scoped_lock lock(runtimeLightMutex);
		frozenNativeLights.clear();
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

void DX12NativeScene::LoadSettings(json& input)
{
	shadowMaskEnabled = input.value("ShadowMask", true);
	outdoorShadowOffloading = input.value("OutdoorShadowOffloading", false);
	previewRuntimeShadows.store(input.value("PreviewRuntimeShadows", false), std::memory_order_relaxed);
	densityMode = std::min(input.value("ShadowTraceDensityMode", 7u), 7u);
	freezeNativeFlickering = input.value("FreezeNativeFlickering", false);
	{
		std::scoped_lock lock(runtimeLightMutex);
		frozenNativeLights.clear();
	}
	nearLightShadowExclusion = input.value("ShadowNearLightExclusion", true);
	nearLightExclusionUnits = std::clamp(input.value("ShadowNearLightRadius", 16u), 0u, 512u) / 4u * 4u;
	shadowReachMultiplier = std::clamp(input.value("ShadowReachMultiplier", 1.0f), 0.1f, 64.0f);
	shadowFadeRange = std::round(std::clamp(input.value("ShadowFadeRange", 0.66f), 0.0f, 1.0f) * 50.0f) / 50.0f;
	shadowFalloffCurve = std::clamp(input.value("ShadowFalloffCurve", 1.0f), 0.0f, 2.5f);
}

void DX12NativeScene::SaveSettings(json& output)
{
	output = { { "ShadowMask", shadowMaskEnabled }, { "OutdoorShadowOffloading", outdoorShadowOffloading }, { "ShadowTraceDensityMode", densityMode },
		{ "PreviewRuntimeShadows", previewRuntimeShadows.load(std::memory_order_relaxed) },
		{ "FreezeNativeFlickering", freezeNativeFlickering },
		{ "ShadowNearLightExclusion", nearLightShadowExclusion },
		{ "ShadowNearLightRadius", nearLightExclusionUnits },
		{ "ShadowReachMultiplier", shadowReachMultiplier },
		{ "ShadowFadeRange", shadowFadeRange },
		{ "ShadowFalloffCurve", shadowFalloffCurve } };
}

void DX12NativeScene::RestoreDefaultSettings()
{
	shadowMaskEnabled = true;
	outdoorShadowOffloading = false;
	previewRuntimeShadows.store(false, std::memory_order_relaxed);
	densityMode = 7;
	freezeNativeFlickering = false;
	{
		std::scoped_lock lock(runtimeLightMutex);
		frozenNativeLights.clear();
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

bool DX12NativeScene::OwnsStaticForLight(RE::BSShadowLight*, int32_t) const
{
	// Observed geometry does not establish complete per-light caster coverage.
	return false;
}

void DX12NativeScene::ObserveRuntimeLight(RE::BSShadowLight* light, uint32_t referenceID,
	bool flickers, const RE::NiPoint3& restOrigin)
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive() ||
		!light || light->GetIsFrustumOrDirectionalLight() || !globals::game::graphicsState)
		return;
	auto* ni = light->light.get();
	if (!ni || !globals::features::dx12Control.IsShadowOffloadMode())
		return;
	const auto& rt = ni->GetLightRuntimeData();
	const auto& position = ni->world.translate;
	if (!std::isfinite(rt.radius.x) || rt.radius.x <= 0.0f ||
		!std::isfinite(rt.fade) || !std::isfinite(rt.diffuse.red) ||
		!std::isfinite(rt.diffuse.green) || !std::isfinite(rt.diffuse.blue) ||
		!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
		return;

	DX12::PreparedLight live{};
	live.referenceID = referenceID;
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
	if (freezeNativeFlickering && flickers && referenceID != 0) {
		// Keep DX11's flicker intact. Only the native shadow has a stable light.
		if (std::isfinite(restOrigin.x) && std::isfinite(restOrigin.y) && std::isfinite(restOrigin.z))
			live.position = float3{ restOrigin.x, restOrigin.y, restOrigin.z };
		if (frozenNativeLights.size() < DX12Native::kMaxShadowLights || frozenNativeLights.contains(referenceID))
			live = frozenNativeLights.try_emplace(referenceID, live).first->second;
	}
	// Deduplicate repeated observations of the same engine light.
	const auto it = std::ranges::find_if(runtimeLightSamples, [ni](const auto& sample) {
		return sample.source == ni;
	});
	if (it != runtimeLightSamples.end()) {
		it->referenceID = referenceID;
		it->value = live;
		it->flickers = flickers;
	} else if (runtimeLightSamples.size() < DX12Native::kMaxShadowLights) {
		runtimeLightSamples.push_back({ ni, referenceID, live, flickers });
	}
}

void DX12NativeScene::PreShadowMaps()
{
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive() || !globals::features::dx12Control.IsShadowOffloadMode()) {
		shadowLights.store(nullptr, std::memory_order_release);
		return;
	}
	auto grid = globals::features::dx12Resources.GetGrid();
	if (!grid || !grid->requested.front().scope) {
		shadowLights.store(nullptr, std::memory_order_release);
		return;
	}
	// Only a revision-coalesced mailbox handoff on this thread. All BLAS/TLAS
	// allocations, command recording and GPU builds run on their own worker.
	if (acceleration.Start(globals::d3d::device, globals::d3d::context))
		acceleration.Request(std::move(grid));
}

void DX12NativeScene::OnOpaqueRasterReady(const OpaqueRasterInputs& /*a_inputs*/)
{
	CS_GPU_PASS("DX12NativeScene::OpaqueRasterReady");
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive())
		return;
	if (!globals::game::graphicsState || !globals::state || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor &
			static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return;
	// Only capture the depth and camera used by the independent shadow layer.
	if (!shadowMaskEnabled || !globals::features::dx12Control.IsShadowOffloadMode() ||
		!CanOffloadShadowsInCurrentCell())
		return;

	if (!backend) {
		std::string reason;
		backend = GraphicsBackend::Create(globals::d3d::device, globals::d3d::context, reason);
		if (!backend) {
			return;
		}
	}

	const uint32_t frame = globals::game::graphicsState->GetFrameCount();
	raster.Capture(*backend, Util::GetCurrentSceneDepthSRV(false),
		&globals::game::frameBufferCached.nonVR, frame);
	// The active view may occupy only a subrect of the full-size raster allocation.
	const auto viewport = Util::ConvertToDynamic(globals::state->screenSize);
	const uint32_t viewportWidth = std::isfinite(viewport.x) && viewport.x > 0.0f ? uint32_t(viewport.x) : 0u;
	const uint32_t viewportHeight = std::isfinite(viewport.y) && viewport.y > 0.0f ? uint32_t(viewport.y) : 0u;
	{
		const auto captured = raster.Get(frame);
		const auto built = acceleration.GetScene();
		// The native trace handles unavailable inputs without affecting raster
		// rendering. RayQuery dispatch remains in DX12NativeShadowTrace.
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
		std::vector<DX12::PreparedLight> liveLights;
		{
			std::scoped_lock lock(runtimeLightMutex);
			if (runtimeLightFrame == frame) {
				liveLights.reserve(runtimeLightSamples.size());
				for (const auto& runtime : runtimeLightSamples)
					liveLights.push_back(runtime.value);
			}
		}
		const auto grid = globals::features::dx12Resources.GetGrid();
		if (!built || !built->inputs || !built->inputs->residency || !grid ||
			!DX12::RuntimeSceneMatches(built->scope, built->inputs->residency->runtimeEpoch,
				grid->requested.front().scope, grid->runtimeEpoch))
			return;
		shadowLights.store(std::make_shared<const std::vector<DX12::PreparedLight>>(liveLights), std::memory_order_release);
		shadows.Dispatch(*backend, captured, built, liveLights, frame, controls);
	}
}

std::function<void()> DX12NativeScene::OnRenderPassBegin(const RE::BSRenderPass* pass)
{
	// Observe visible static geometry already drawn by the existing opaque
	// render path. This does not change render-pass state or DX11 draw behavior.
	if (!pass || !pass->geometry || !loaded || globals::game::isVR ||
		!shadowMaskEnabled || !globals::features::dx12Control.IsActive() ||
		!globals::features::dx12Control.IsShadowOffloadMode() ||
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
	if (!loaded || globals::game::isVR || !globals::features::dx12Control.IsActive() || !globals::features::dx12Control.IsShadowOffloadMode())
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

void DX12NativeScene::OnSceneTransitionReset(bool a_opening)
{
	if (a_opening) {
		shadowLights.store(nullptr, std::memory_order_release);
		{
			std::scoped_lock lock(runtimeLightMutex);
			runtimeLightFrame = UINT32_MAX;
			runtimeLightSamples.clear();
			frozenNativeLights.clear();
		}
		raster.Reset();
		shadows.Reset();
		acceleration.Reset();
		geometryCapture.Reset();
	}
}

void DX12NativeScene::OnRuntimeDisabled()
{
	shadowLights.store(nullptr, std::memory_order_release);
	{
		std::scoped_lock lock(runtimeLightMutex);
		runtimeLightFrame = UINT32_MAX;
		runtimeLightSamples.clear();
		frozenNativeLights.clear();
	}
	raster.Reset();
	shadows.Reset();
	acceleration.Reset();
	geometryCapture.Reset();
}

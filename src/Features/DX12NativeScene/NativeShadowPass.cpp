#include "DX12NativeScene.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Util.h"
#include <cmath>

void DX12NativeScene::SanitizeShadowControls()
{
	const auto clamp = [](float value, float fallback, float minimum, float maximum) {
		return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
	};
	settings.ShadowStrength = clamp(settings.ShadowStrength, 0, 0, 1);
	settings.ShadowBlend = clamp(settings.ShadowBlend, 1, 0, 1);
	settings.ShadowRadius = clamp(settings.ShadowRadius, 0, 0, 32);
	settings.ShadowReceiverTolerance = clamp(settings.ShadowReceiverTolerance, 2, 0.1f, 64);
	settings.ShadowLightIndex = std::min(settings.ShadowLightIndex, 31u);
	settings.ShadowEmitterRadius = clamp(settings.ShadowEmitterRadius, 1, 0, 8);
	settings.ShadowHistoryWeight = clamp(settings.ShadowHistoryWeight, 0.70f, 0, 0.95f);
	settings.ShadowReachMultiplier = clamp(settings.ShadowReachMultiplier, 1, 0.1f, 64);
	settings.ShadowFalloffFraction = clamp(settings.ShadowFalloffFraction, 0.1f, 0.02f, 1);
	settings.ShadowFalloffExponent = clamp(settings.ShadowFalloffExponent, 1, 0.25f, 4);
	settings.ShadowTraceResolution = std::min(settings.ShadowTraceResolution, 4u);
	settings.ShadowNormalInfluence = std::min(settings.ShadowNormalInfluence, 2u);
}

void DX12NativeScene::DispatchNativeShadowMask(const char* stage)
{
	CS_GPU_PASS("DX12NativeScene::EarlyRTShadowMask");
	if (!globals::game::graphicsState)
		return;
	const auto frame = globals::game::graphicsState->GetFrameCount();
	nativeShadowTrace.DiscardSceneOutputsBefore(frame);
	nativeShadowTrace.Retire();
	if (!settings.NativeShadowMask) {
		shadowMaskUnavailable = "Disabled";
		return;
	}
	if (nativeShadowTrace.GetShadowMaskOutput(frame))
		return;
	shadowMaskStage = stage;
	if (globals::game::isVR || transitionOpen || globals::state->isMapMenuOpen ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))) {
		shadowMaskUnavailable = "Main non-VR world camera unavailable";
		return;
	}
	if (!globals::game::frameBufferCachedValid || globals::game::frameBufferCachedFrame != frame) {
		shadowMaskUnavailable = "Current-frame camera snapshot not ready at this early stage";
		return;
	}
	const auto& projection = globals::game::frameBufferCached.nonVR.CameraProj;
	if (!std::isfinite(projection._44) || std::abs(projection._44) > 1e-5f) {
		shadowMaskUnavailable = "Waiting for the main perspective camera; orthographic shadow cameras are not receivers";
		return;
	}
	const auto* worldCamera = RE::Main::WorldRootCamera();
	if (!worldCamera) { shadowMaskUnavailable = "Main world camera unavailable"; return; }
	const auto& inverseView = globals::game::frameBufferCached.nonVR.CameraViewInverse;
	const auto& adjustment = globals::game::frameBufferCached.GetCameraPosAdjust();
	const auto& worldOrigin = worldCamera->world.translate;
	const float dx = adjustment.x + inverseView._14 - worldOrigin.x;
	const float dy = adjustment.y + inverseView._24 - worldOrigin.y;
	const float dz = adjustment.z + inverseView._34 - worldOrigin.z;
	constexpr float kCameraOriginToleranceSquared = 0.25f;
	const float errorSquared = dx*dx + dy*dy + dz*dz;
	if (!std::isfinite(errorSquared) || errorSquared > kCameraOriginToleranceSquared) {
		shadowMaskUnavailable = "Cached perspective camera is not the current main world camera; waiting for its upload";
		return;
	}
	const auto publishedScene = GetPublishedScene();
	if (!publishedScene) {
		shadowMaskUnavailable = "Current prepared scene/TLAS unavailable";
		return;
	}
	const auto graphics = Feature::FindGraphicsBackend();
	if (!graphics || !graphics->commandQueue || !graphics->d3d12Device) {
		shadowMaskUnavailable = "Native DX12 backend unavailable";
		return;
	}
	const auto& prepared = publishedScene->cell->nativeLights;
	if (prepared.empty() || (settings.ShadowAllLights && prepared.size() > 32) ||
		(!settings.ShadowAllLights && settings.ShadowLightIndex >= prepared.size())) {
		shadowMaskUnavailable = std::format("Prepared shadow light selection unavailable: {} lights, all={}, selected={}; maximum 32", prepared.size(), settings.ShadowAllLights, settings.ShadowLightIndex);
		return;
	}
	const auto selectedLights = settings.ShadowAllLights ? std::span<const DX12Native::PreparedPointLight>(prepared) :
		std::span<const DX12Native::PreparedPointLight>(prepared).subspan(settings.ShadowLightIndex, 1);
	if (std::ranges::none_of(selectedLights, [](const auto& light) { return light.supported; })) {
		shadowMaskUnavailable = "No eligible prepared static point lights in the shadow selection";
		return;
	}
	const auto dynamicSize = Util::ConvertToDynamic(globals::state->screenSize);
	const std::array<uint32_t, 2> size{ uint32_t(dynamicSize.x), uint32_t(dynamicSize.y) };
	if (!nativeShadowTrace.Initialize(*graphics, true)) {
		shadowMaskUnavailable = nativeShadowTrace.GetDiagnostics().value("failure", std::string("Shadow pipeline unavailable"));
		return;
	}
	nativeShadowTrace.Dispatch(publishedScene->acceleration, size, {}, frame, uint64_t(settings.ShadowMaskBudgetMB) * 1024u * 1024u,
		true, {}, 0u, nullptr, 0, 0, nullptr, {}, settings.ShadowAllLights, { 1, 1, 1, 1 }, false, 1.0f, false, 0, {}, 1.0f, settings.ShadowNormalInfluence, selectedLights, settings.ShadowTraceResolution, 0u, 3u, false, 0.2f,
		{ settings.ShadowReachMultiplier, settings.ShadowFalloffFraction, settings.ShadowFalloffExponent, settings.ShadowDistanceFade ? 1.0f : 0.0f },
		{ settings.ShadowStochastic ? 1.0f : 0.0f, settings.ShadowEmitterRadius, 0, 0 }, true);
	if (nativeShadowTrace.GetShadowMaskOutput(frame))
		shadowMaskUnavailable.clear();
	else {
		const auto status = nativeShadowTrace.GetDiagnostics();
		shadowMaskUnavailable = status.value("failure", std::string{});
		if (shadowMaskUnavailable.empty())
			shadowMaskUnavailable = "Shadow bindings warming, frame slots busy or memory admission deferred; no current mask";
	}
}

std::optional<DX12Native::DirectionalTrace::ShadowMaskOutput> DX12NativeScene::GetNativeShadowMask(uint32_t frame) const
{
	if (!settings.NativeShadowMask || transitionOpen)
		return std::nullopt;
	return nativeShadowTrace.GetShadowMaskOutput(frame);
}

void DX12NativeScene::ReleaseNativeShadowMask(uint32_t frame, ID3D12Fence* consumer, uint64_t value)
{
	nativeShadowTrace.ReleaseSceneOutput(frame, consumer, value);
}

#include "DX12NativeScene.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Util.h"
#include <cmath>

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
	auto* accumulator = *globals::game::currentAccumulator.get();
	auto* node = accumulator ? accumulator->GetRuntimeData().activeShadowSceneNode : nullptr;
	auto* sun = node ? node->GetRuntimeData().sunLight : nullptr;
	auto* light = sun ? skyrim_cast<RE::NiDirectionalLight*>(sun->light.get()) : nullptr;
	if (!light) {
		shadowMaskUnavailable = "Current directional light unavailable";
		return;
	}
	const auto direction = light->GetWorldDirection();
	const float lengthSquared = direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;
	if (!std::isfinite(lengthSquared) || lengthSquared <= 1e-8f) {
		shadowMaskUnavailable = "Invalid directional light snapshot";
		return;
	}
	const float inverse = 1.0f / std::sqrt(lengthSquared);
	const float3 toLight{ -direction.x * inverse, -direction.y * inverse, -direction.z * inverse };
	const auto dynamicSize = Util::ConvertToDynamic(globals::state->screenSize);
	const std::array<uint32_t, 2> size{ uint32_t(dynamicSize.x), uint32_t(dynamicSize.y) };
	if (!nativeShadowTrace.Initialize(*graphics, true)) {
		shadowMaskUnavailable = nativeShadowTrace.GetDiagnostics().value("failure", std::string("Shadow pipeline unavailable"));
		return;
	}
	nativeShadowTrace.Dispatch(publishedScene->acceleration, size, toLight, frame, uint64_t(settings.ShadowMaskBudgetMB) * 1024u * 1024u,
		false, {}, 0u, nullptr, 0, 0, nullptr, {}, false, { 1, 1, 1, 1 }, false, 1.0f, false, 0, {}, 1.0f, 2u, {}, 0u, 0u, 3u, false, 0.2f,
		{ 1, 0.1f, 1, 0 }, { 0, 0, 0, 0 }, true);
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

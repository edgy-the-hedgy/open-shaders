#include "RedstoneShadows.h"

#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Deferred.h"
#include "Utils/DevBenchUx.h"
#include "Menu/ThemeManager.h"
#include <chrono>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RedstoneShadows::Settings, AcquireScene, NodesPerFrame, MaxInstances, BufferBudgetMB)

std::pair<std::string, std::vector<std::string>> RedstoneShadows::GetFeatureSummary()
{
	return { "Independent Redstone scene acquisition and partial directional ray tracing. Native shadow production remains active until replacement coverage and lighting consumption are available.", {} };
}

void RedstoneShadows::LoadSettings(json& a_json)
{
	Settings selected = a_json;
	selected.NodesPerFrame = std::clamp(selected.NodesPerFrame, 64u, 8192u);
	selected.MaxInstances = std::clamp(selected.MaxInstances, 128u, 32768u);
	selected.BufferBudgetMB = std::clamp(selected.BufferBudgetMB, 16u, 512u);
	settings = selected;
	resetRequested = true;
}

void RedstoneShadows::SaveSettings(json& a_json)
{
	a_json = settings;
}

void RedstoneShadows::RestoreDefaultSettings()
{
	settings = {};
	resetRequested = true;
}

void RedstoneShadows::SetupResources()
{
	ready = dxrCapable = rrEligible = false;
	resetRequested = true;
	const auto backend = Feature::FindGraphicsBackend();
	if (!backend || !backend->d3d12Device) {
		unavailable = "Existing graphics backend unavailable";
		return;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
	dxrCapable = SUCCEEDED(backend->d3d12Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options))) &&
		options.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
	rrEligible = backend->fsrFrameGeneration && backend->rayRegenerationHardware && !globals::game::isVR;
	if (!rrEligible || !dxrCapable) {
		unavailable = !rrEligible ? "Requires the existing eligible non-VR FSR-FG session" : "DXR 1.1 unsupported by the current device";
		return;
	}
	ready = scene.Initialize();
	unavailable = ready ? "" : "Scene completion query unavailable";
}

void RedstoneShadows::EarlyPrepass()
{
	CS_GPU_PASS("RedstoneShadows::AcquireScene");
	const auto started = std::chrono::steady_clock::now();
	const SKSE::stl::scope_exit measure([&]() noexcept {
		lastCPUms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	});
	const auto frame = globals::game::graphicsState->GetFrameCount();
	if (!ready && settings.AcquireScene && frame % kBackendRetryFrames == 0) SetupResources();
	if (previousFrame && (frame < *previousFrame || frame - *previousFrame > 1)) resetRequested = true;
	previousFrame = frame;
	if (resetRequested) {
		scene.Reset();
		geometryBridge.Reset();
		acceleration.Reset();
		directional.Reset();
		lastTraceFrame.reset();
		resetRequested = false;
	}
	const auto budget = uint64_t(settings.BufferBudgetMB) * 1024 * 1024 / 4;
	const bool copiesReady = scene.RetireCopies();
	if (!ready || !settings.AcquireScene || transitionOpen || globals::state->isMapMenuOpen || !copiesReady) {
		geometryBridge.Advance(nullptr, budget);
		acceleration.Advance(nullptr, geometryBridge, budget);
		return;
	}
	const auto backend = Feature::FindGraphicsBackend();
	if (!backend || !geometryBridge.Initialize(*backend) || !acceleration.Initialize(*backend)) return;
	scene.Advance({ settings.NodesPerFrame, settings.MaxInstances, budget },
		frame);
	geometryBridge.Advance(scene.GetSnapshot(), budget);
	acceleration.Advance(scene.GetSnapshot(), geometryBridge, budget);
}

void RedstoneShadows::OnSceneTransitionReset(bool a_open)
{
	transitionOpen = a_open;
	resetRequested = true;
}

json RedstoneShadows::GetDiagnostics()
{
	const auto directionalStatus = directional.GetDiagnostics();
	return {
		{ "dxrCapable", dxrCapable }, { "rrEligible", rrEligible }, { "resourcesReady", ready },
		{ "acquisitionEnabled", settings.AcquireScene }, { "unavailable", unavailable },
		{ "scene", scene.GetDiagnostics() },
		{ "lastSceneCPUms", lastCPUms }, { "lastDirectionalCPUms", directionalCPUms }, { "lastTotalCPUms", lastCPUms + directionalCPUms },
		{ "geometryTransport", geometryBridge.GetDiagnostics() },
		{ "acceleration", acceleration.GetDiagnostics() },
		{ "directional", directionalStatus },
		{ "ownership", { { "directional", "native" }, { "local", "native/LLF/SCM" }, { "contact", "native" }, { "distant", "native/LOD" } } },
		{ "traversalSubmitted", directionalStatus.at("submissions").get<uint64_t>() != 0 }, { "rrSubmitted", false },
		{ "outputConsumed", false }, { "debugPreviewDrawSubmitted", directionalStatus.at("previewDrawSubmissions").get<uint64_t>() != 0 },
		{ "cpuProductionBypassed", false }, { "fallbackReason", "Partial opaque traversal does not establish production coverage; scene-lighting replacement remains unavailable" }
	};
}

void RedstoneShadows::DrawSettings()
{
	if (ImGui::Checkbox("Acquire shadow scene", &settings.AcquireScene)) resetRequested = true;
	if (ImGui::Checkbox("Preview directional visibility (partial)", &previewDirectional)) traceDirectional = previewDirectional;
	ImGui::TextWrapped("Native directional, local, contact and distant shadow production remains active. Acquisition does not establish ray-traced shadow coverage.");
	if (!unavailable.empty()) ImGui::TextWrapped("%s", unavailable.c_str());
	ImGui::Text("DXR: %s; RR session: %s", dxrCapable ? "supported" : "unavailable", rrEligible ? "eligible" : "ineligible");
	int nodes = int(settings.NodesPerFrame), instances = int(settings.MaxInstances), budget = int(settings.BufferBudgetMB);
	bool changed = ImGui::SliderInt("Node observations per frame", &nodes, 64, 8192);
	changed |= ImGui::SliderInt("Maximum shadow instances", &instances, 128, 32768);
	changed |= ImGui::SliderInt("Geometry buffer budget (MiB)", &budget, 16, 512);
	if (changed) {
		settings.NodesPerFrame = uint32_t(nodes);
		settings.MaxInstances = uint32_t(instances);
		settings.BufferBudgetMB = uint32_t(budget);
		resetRequested = true;
	}
}

void RedstoneShadows::RegisterUxActions()
{
	FEATURE_QUERY("shadowStatus",
		"Return independent RedstoneShadows DXR 1.1 and RR eligibility, bounded GPU acquisition/export/BLAS/TLAS state, raw directional traversal and preview evidence, unsupported caster reasons and native ownership. Scene-lighting consumption and CPU bypass remain separate from debug consumption. No params.",
		[](const Feature* a_self, const json&) { return const_cast<RedstoneShadows*>(static_cast<const RedstoneShadows*>(a_self))->GetDiagnostics(); });
	FEATURE_COMMAND("setSceneAcquisition",
		"Enable or disable bounded off-screen shadow scene acquisition and partial opaque acceleration builds. Params: enabled (required boolean). Resets scene generation. Native production remains active. Unsaved TraceDirectional/PreviewDirectional runtime flags separately request tracing/debug display; this command does not enable RR. Normal Save persists acquisition.",
		[](Feature* a_self, const json& a_args) {
			auto* shadows = static_cast<RedstoneShadows*>(a_self);
			shadows->settings.AcquireScene = a_args.at("enabled").get<bool>();
			shadows->resetRequested = true;
		});
	FEATURE_COMMAND("resetShadowScene",
		"Invalidate the owned shadow scene and stable identities at the next render boundary. No params. Does not reset RedstoneGI or change native shadow ownership.",
		[](Feature* a_self, const json&) { static_cast<RedstoneShadows*>(a_self)->resetRequested = true; });
}

void RedstoneShadows::OnBeforePostProcessing(RE::RENDER_TARGET)
{
	CS_GPU_PASS("RedstoneShadows::FinalReceivers");
	const auto frame = globals::game::graphicsState->GetFrameCount();
	if (!directionalCPUFrame || *directionalCPUFrame != frame) { directionalCPUms = 0; directionalCPUFrame = frame; }
	const auto started = std::chrono::steady_clock::now();
	const SKSE::stl::scope_exit measure([&]() noexcept {
		directionalCPUms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	});
	directional.Retire();
	if (!globals::state->worldRenderedThisFrame || !traceDirectional || !settings.AcquireScene || !ready || transitionOpen ||
		globals::game::isVR || globals::state->isMapMenuOpen || !globals::deferred->sceneDepthFinal) return;
	if (lastTraceFrame && *lastTraceFrame == frame) return;
	lastTraceFrame = frame;
	const auto backend = Feature::FindGraphicsBackend();
	if (!backend || !directional.Initialize(*backend)) return;
	auto tlas = acceleration.GetScene();
	if (!tlas) return;
	const auto sceneNode = globals::game::smState->shadowSceneNode[0];
	const auto sun = sceneNode ? sceneNode->GetRuntimeData().sunLight : nullptr;
	const auto light = sun ? skyrim_cast<RE::NiDirectionalLight*>(sun->light.get()) : nullptr;
	if (!light) return;
	const auto engineDirection = light->GetWorldDirection();
	float3 direction{ -engineDirection.x, -engineDirection.y, -engineDirection.z };
	const auto length = direction.Length();
	constexpr float kMinimumDirectionLength = 1e-6f;
	if (!std::isfinite(length) || length < kMinimumDirectionLength) return;
	direction /= length;
	const auto size = Util::ConvertToDynamic(globals::state->screenSize);
	if (!std::isfinite(size.x) || !std::isfinite(size.y) || size.x < 1 || size.y < 1 || size.x > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || size.y > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) return;
	directional.Dispatch(std::move(tlas), { uint32_t(size.x), uint32_t(size.y) }, direction, frame, uint64_t(settings.BufferBudgetMB) * 1024 * 1024 / 4);
}

json RedstoneShadows::GetRuntimeFlags()
{
	return { { "TraceDirectional", traceDirectional }, { "PreviewDirectional", previewDirectional } };
}

bool RedstoneShadows::SetRuntimeFlag(std::string_view a_name, bool a_value)
{
	if (a_name == "TraceDirectional") traceDirectional = a_value;
	else if (a_name == "PreviewDirectional") {
		previewDirectional = a_value;
		if (a_value) traceDirectional = true;
	} else return false;
	return true;
}

void RedstoneShadows::DrawOverlay()
{
	if (!IsOverlayVisible()) return;
	const auto* viewport = ImGui::GetMainViewport();
	const ImVec2 end{ viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y };
	auto* draw = ImGui::GetBackgroundDrawList();
	draw->AddRectFilled(viewport->Pos, end, IM_COL32(0, 0, 0, 255));
	if (auto* view = directional.Preview()) {
		draw->AddImage(reinterpret_cast<ImTextureID>(view), viewport->Pos, end);
		directional.MarkPreviewDrawSubmitted();
	}
	const float kLabelInset = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * Util::GetUIScale();
	const auto status = directional.GetDiagnostics();
	const auto label = std::format("Raw directional visibility | partial opaque scene | source frame {} | white exposed, black blocked, red invalid | native production active", status.at("previewFrame").get<uint32_t>());
	draw->AddText({ viewport->Pos.x + kLabelInset, viewport->Pos.y + kLabelInset }, IM_COL32_WHITE,
		label.c_str());
}

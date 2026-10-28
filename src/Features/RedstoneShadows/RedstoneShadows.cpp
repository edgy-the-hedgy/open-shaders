#include "RedstoneShadows.h"

#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/DevBenchUx.h"
#include "Utils/Region.h"
#include "Util.h"
#include <chrono>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RedstoneShadows::Settings, AcquireScene, UsePreparedScene, PreviewPreparedScene, PreparedLight, NodesPerFrame, MaxInstances, BufferBudgetMB)

std::pair<std::string, std::vector<std::string>> RedstoneShadows::GetFeatureSummary()
{
	return { "Offline cell geometry feeds native DX12 acceleration and an opt-in baseline local-light preview. SCM observation remains separate. Native shadow production remains active.", {} };
}

void RedstoneShadows::LoadSettings(json& a_json)
{
	Settings selected = a_json;
	selected.NodesPerFrame = std::clamp(selected.NodesPerFrame, 64u, 8192u);
	selected.MaxInstances = std::clamp(selected.MaxInstances, 1u, CasterObservation::kCapacity);
	selected.BufferBudgetMB = std::clamp(selected.BufferBudgetMB, 16u, 512u);
	selected.PreparedLight = std::min(selected.PreparedLight, 255u);
	if (selected.UsePreparedScene != settings.UsePreparedScene) ready = false;
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
	ready = false;
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
	ready = settings.UsePreparedScene || scene.Initialize();
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
	if (!ready && (settings.AcquireScene || settings.UsePreparedScene) && frame % kBackendRetryFrames == 0) SetupResources();
	if (previousFrame && (frame < *previousFrame || frame - *previousFrame > 1)) resetRequested = true;
	previousFrame = frame;
	const auto budget = uint64_t(settings.BufferBudgetMB) * 1024 * 1024 / 4;
	preparedAcceleration.Advance();
	preparedTrace.Retire();
	if (settings.UsePreparedScene) {
		CasterObservation::SetEnabled(false);
		if (acquisitionActive) { acquisitionActive = false; scene.Reset(); geometryBridge.Reset(); acceleration.Reset(); }
		acceleration.Advance();
		geometryBridge.Advance(nullptr, budget);
		scene.RetireCopies();
		AdvancePreparedScene(frame, budget);
		return;
	}
	std::optional<GraphicsBackend> backend;
	if (ready && settings.AcquireScene && !transitionOpen && !globals::game::isVR && !globals::state->isMapMenuOpen)
		backend = Feature::FindGraphicsBackend();
	bool active = backend && backend->commandQueue && !scene.Failed();
	if (active && !geometryBridge.Initialize(*backend)) {
		active = false;
		unavailable = "Geometry export unavailable; native shadow production remains active";
	}
	if (active != acquisitionActive) {
		acquisitionActive = active;
		resetRequested = true;
	}
	CasterObservation::SetEnabled(active);
	if (observedEpoch != CasterObservation::Epoch()) resetRequested = true;
	if (resetRequested) {
		ResetPreparedScene();
		CasterObservation::Invalidate();
		observedEpoch = CasterObservation::Epoch();
		scene.Reset();
		geometryBridge.Reset();
		acceleration.Reset();
		preparedGeometry.clear();
		preparingBatch = workerBatch = 0;
		debugBoundCount = 0;
		debugScope = {};
		debugStatus = {};
		exportProgress = UINT64_MAX;
		resetRequested = false;
	}
	const bool copiesReady = scene.RetireCopies();
	acceleration.Advance();
	if (!active || !copiesReady) {
		if (captureDebug) UpdateCaptureDebug();
		geometryBridge.Advance(nullptr, budget);
		if (!active) {
			for (uint32_t i = 0; i < CasterObservation::kQueueCapacity; ++i) {
				auto* queued = CasterObservation::Acquire();
				if (!queued) break;
				CasterObservation::Release(queued);
			}
		}
		return;
	}
	try {
		AdvanceAcquisition(*backend, frame, budget);
		if (captureDebug) UpdateCaptureDebug();
	} catch (const std::exception& error) {
		unavailable = error.what();
		ready = acquisitionActive = false;
		CasterObservation::SetEnabled(false);
		resetRequested = true;
	}
}

void RedstoneShadows::AdvanceAcquisition(const GraphicsBackend& a_backend, uint64_t a_frame, uint64_t a_budgetBytes)
{
	if (workerBatch && acceleration.Finished()) {
		if (captureDebug) UpdateCaptureDebug();
		scene.ReleaseSnapshot();
		workerBatch = preparingBatch = 0;
		preparedGeometry.clear();
	}
	scene.Advance({ settings.MaxInstances, a_budgetBytes }, a_frame);
	const auto* snapshot = scene.GetSnapshot();
	const auto deferrals = geometryBridge.BudgetDeferrals();
	geometryBridge.Advance(workerBatch ? nullptr : snapshot, a_budgetBytes);
	if (!snapshot || workerBatch) return;
	if (snapshot->scope.scene != CasterObservation::Epoch() || geometryBridge.Failed() || geometryBridge.BudgetDeferrals() != deferrals) {
		++discardedBatches;
		lastDiscardBatch = snapshot->scope.batch;
		if (snapshot->scope.scene != CasterObservation::Epoch()) ++preparingEpochDiscards;
		lastDiscardReason = snapshot->scope.scene != CasterObservation::Epoch() ? "Scene epoch invalidated" :
			geometryBridge.Failed() ? "Geometry export failed" : "Geometry export budget/layout deferral";
		scene.ReleaseSnapshot();
		preparedGeometry.clear();
		preparingBatch = 0;
		geometryBridge.Reset();
		return;
	}
	if (preparingBatch == snapshot->sweep && exportProgress == geometryBridge.Progress()) return;
	if (preparingBatch != snapshot->sweep) preparedGeometry.clear();
	preparingBatch = snapshot->sweep;
	exportProgress = geometryBridge.Progress();
	bool complete = true;
	for (const auto& instance : snapshot->instances) {
		if (preparedGeometry.contains(instance.mesh.get())) continue;
		if (auto geometry = geometryBridge.Find(instance.mesh.get())) preparedGeometry.emplace(instance.mesh.get(), std::move(geometry));
		else complete = false;
	}
	if (!complete) return;
	workerBatch = snapshot->sweep;
	acceleration.Start(a_backend, scene.RetainSnapshot(), std::move(preparedGeometry), a_budgetBytes);
	acceleration.Advance();
}

void RedstoneShadows::OnSceneTransitionReset(bool a_open)
{
	transitionOpen = a_open;
	resetRequested = true;
}

void RedstoneShadows::OnRuntimeDisabled()
{
	ResetPreparedScene();
	preparedAcceleration.Advance();
	preparedTrace.Retire();
	CasterObservation::SetEnabled(false);
	acquisitionActive = false;
	acceleration.Reset();
	acceleration.Advance();
	resetRequested = true;
	for (uint32_t i = 0; i < CasterObservation::kQueueCapacity; ++i) {
		auto* queued = CasterObservation::Acquire();
		if (!queued) break;
		CasterObservation::Release(queued);
	}
}

json RedstoneShadows::GetDiagnostics()
{
	auto accelerationStatus = acceleration.GetDiagnostics();
	const auto traceStatus = preparedTrace.GetDiagnostics();
	const auto* snapshot = scene.GetSnapshot();
	if (!acquisitionActive || !snapshot || accelerationStatus.value("sourceBatch", uint64_t{ 0 }) != snapshot->scope.batch)
		accelerationStatus["publicationMatchesBatch"] = false;
	return {
		{ "dxrCapable", dxrCapable }, { "rrEligible", rrEligible }, { "resourcesReady", ready },
		{ "acquisitionEnabled", settings.AcquireScene }, { "unavailable", unavailable },
		{ "preparedSceneEnabled", settings.UsePreparedScene }, { "preparedScene", preparedScene.GetDiagnostics() },
		{ "preparedAcceleration", preparedAcceleration.GetDiagnostics() }, { "preparedTrace", traceStatus },
		{ "scene", scene.GetDiagnostics() },
		{ "observation", CasterObservation::Diagnostics() }, { "acquisitionActive", acquisitionActive },
		{ "lastRenderThreadCPUms", lastCPUms }, { "discardedBatches", discardedBatches },
		{ "lastDiscardBatch", lastDiscardBatch }, { "lastDiscardReason", lastDiscardReason },
		{ "preparingEpochDiscards", preparingEpochDiscards },
		{ "preparingBatch", preparingBatch }, { "workerBatch", workerBatch }, { "preparedMeshes", preparedGeometry.size() },
		{ "stageBudgetBytes", uint64_t(settings.BufferBudgetMB) * 1024 * 1024 / 4 },
		{ "geometryTransport", geometryBridge.GetDiagnostics() },
		{ "acceleration", accelerationStatus },
		{ "directional", { { "available", false }, { "reason", "Observed local-light batches cannot supply directional coverage" } } },
		{ "ownership", { { "directional", "native" }, { "local", "native/LLF/SCM" }, { "contact", "native" }, { "distant", "native/LOD" } } },
		{ "traversalSubmitted", traceStatus.value("submissions", 0ull) != 0 }, { "rrSubmitted", false },
		{ "outputConsumed", false }, { "debugPreviewDrawSubmitted", traceStatus.value("previewDrawSubmissions", 0ull) != 0 }, { "captureDebugEnabled", captureDebug.load() },
		{ "cpuProductionBypassed", false }, { "fallbackReason", "Prepared and observed scenes are diagnostic inputs; native shadow consumers remain authoritative" }
	};
}

void RedstoneShadows::DrawSettings()
{
	if (ImGui::Checkbox("Use prepared cell geometry", &settings.UsePreparedScene)) { ready = false; resetRequested = true; }
	if (ImGui::Checkbox("Prepared local-light shadow preview", &settings.PreviewPreparedScene)) { preparedTrace.Reset(); }
	const auto prepared = preparedScene.GetDiagnostics();
	int light = int(settings.PreparedLight);
	const auto maximumLight = std::max(0, int(prepared.value("lightCount", 0ull)) - 1);
	if (ImGui::SliderInt("Prepared baseline light", &light, 0, maximumLight)) { settings.PreparedLight = uint32_t(light); preparedTrace.Reset(); }
	ImGui::TextWrapped("Prepared geometry loads once per cell on a worker. The preview uses a baseline light and excludes unresolved materials. Native shadows remain active; saved/moved objects and animation are not validated.");
	ImGui::Text("Prepared meshes: %llu | instances: %llu | material-masked: %u", prepared.value("meshes", 0ull), prepared.value("instances", 0ull), prepared.value("maskedInstances", 0u));
	if (prepared.contains("failure") && !prepared.at("failure").get_ref<const std::string&>().empty())
		ImGui::TextWrapped("%s", prepared.at("failure").get_ref<const std::string&>().c_str());
	if (ImGui::Checkbox("Observe SCM shadow casters", &settings.AcquireScene)) resetRequested = true;
	ImGui::TextWrapped("Observes one rotating local-light slot only when SCM updates it. Native shadows remain active. Directional tracing and preview are unavailable for this partial local-light scope.");
	bool showCaptureDebug = captureDebug.load();
	if (ImGui::Checkbox("Capture diagnostics (temporary overlay)", &showCaptureDebug)) {
		captureDebug.store(showCaptureDebug);
		CasterObservation::SetDiagnosticsEnabled(showCaptureDebug);
		debugBoundCount = 0;
		debugScope = {};
		debugStatus = {};
	}
	ImGui::TextWrapped("Diagnostic boxes enclose captured world bounds, including hidden objects. They are not mesh wireframes or ray-traced shadows.");
	if (!unavailable.empty()) ImGui::TextWrapped("%s", unavailable.c_str());
	ImGui::Text("DXR: %s; RR session: %s", dxrCapable ? "supported" : "unavailable", rrEligible ? "eligible" : "ineligible");
	int instances = int(settings.MaxInstances), budget = int(settings.BufferBudgetMB);
	bool changed = ImGui::SliderInt("Maximum admitted instances", &instances, 1, int(CasterObservation::kCapacity));
	changed |= ImGui::SliderInt("Geometry buffer budget (MiB)", &budget, 16, 512);
	if (changed) {
		settings.MaxInstances = uint32_t(instances);
		settings.BufferBudgetMB = uint32_t(budget);
		resetRequested = true;
	}
}

void RedstoneShadows::RegisterUxActions()
{
	FEATURE_QUERY("shadowStatus",
		"Return RedstoneShadows SCM counters plus prepared-cell loading, native geometry bytes, masked material instances, prepared BLAS/TLAS progress and baseline local-light trace/preview submissions. Prepared package/runtime coverage, RR, production consumption and native CPU bypass remain unvalidated. Trace counters are historical; raw preview draws do not imply lighting replacement. No params.",
		[](const Feature* a_self, const json&) { return const_cast<RedstoneShadows*>(static_cast<const RedstoneShadows*>(a_self))->GetDiagnostics(); });
	FEATURE_COMMAND("setPreparedScene",
		"Enable offline prepared-cell geometry and optional diagnostic local-light tracing. Params: enabled (required boolean), preview (optional boolean), light (optional integer 0..255). Reads Data/SKSE/Plugins/CommunityShaders/RedstoneScene/cells/<plugin>/<local-id>/cell-layout.json on a worker. Unresolved material instances enter BLAS/TLAS but are masked from tracing. Native shadows and GI remain authoritative; normal Save persists selections.",
		[](Feature* a_self, const json& a_args) {
			auto* shadows = static_cast<RedstoneShadows*>(a_self);
			shadows->settings.UsePreparedScene = a_args.at("enabled").get<bool>();
			if (a_args.contains("preview")) shadows->settings.PreviewPreparedScene = a_args.at("preview").get<bool>();
			if (a_args.contains("light")) shadows->settings.PreparedLight = std::clamp(a_args.at("light").get<uint32_t>(), 0u, 255u);
			shadows->ready = false;
			shadows->resetRequested = true;
		});
	FEATURE_COMMAND("setSceneAcquisition",
		"Enable or disable bounded SCM local-light caster observation and asynchronous opaque acceleration builds. Params: enabled (required boolean). Resets observation generation. Does not force accumulation, scan the scene, trace directional shadows, submit RR or replace native rendering. Normal Save persists acquisition.",
		[](Feature* a_self, const json& a_args) {
			auto* shadows = static_cast<RedstoneShadows*>(a_self);
			shadows->settings.AcquireScene = a_args.at("enabled").get<bool>();
			shadows->resetRequested = true;
		});
	FEATURE_COMMAND("resetShadowScene",
		"Invalidate the owned shadow scene and stable identities at the next render boundary. No params. Does not reset RedstoneGI or change native shadow ownership.",
		[](Feature* a_self, const json&) { static_cast<RedstoneShadows*>(a_self)->resetRequested = true; });
}

json RedstoneShadows::GetRuntimeFlags()
{
	return { { "CaptureDebug", captureDebug.load() }, { "TraceDirectional", false }, { "PreviewDirectional", false } };
}

bool RedstoneShadows::SetRuntimeFlag(std::string_view a_name, bool a_value)
{
	if (a_name == "CaptureDebug") {
		captureDebug.store(a_value);
		CasterObservation::SetDiagnosticsEnabled(a_value);
		return true;
	}
	return (a_name == "TraceDirectional" || a_name == "PreviewDirectional") && !a_value;
}

bool RedstoneShadows::IsOverlayVisible() const
{
	return (captureDebug.load() || (settings.UsePreparedScene && settings.PreviewPreparedScene)) && !globals::game::isVR;
}

void RedstoneShadows::UpdateCaptureDebug()
{
	const auto* snapshot = scene.GetDebugSnapshot();
	if (!snapshot || snapshot->scope.scene != CasterObservation::Epoch()) return;
	debugScope = snapshot->scope;
	debugBoundCount = static_cast<uint32_t>(std::min(snapshot->instances.size(), debugBounds.size()));
	for (uint32_t i = 0; i < debugBoundCount; ++i) debugBounds[i] = snapshot->instances[i].bound;
	debugStatus = GetDiagnostics();
	debugStatus["omissions"] = snapshot->omissions;
	debugStatus["incomplete"] = snapshot->truncated;
}

void RedstoneShadows::DrawOverlay()
{
	if (!IsOverlayVisible()) return;
	if (settings.UsePreparedScene && settings.PreviewPreparedScene) {
		CS_GPU_PASS("RedstoneShadows::PreparedPreviewDraw");
		if (ImGui::Begin("Prepared cell shadow preview###RedstonePreparedPreview")) {
			ImGui::TextUnformatted("Baseline local-light visibility | incomplete materials | native shadows active");
			if (auto* image = preparedTrace.Preview()) {
				const auto width = std::max(ImGui::GetContentRegionAvail().x, 128.0f);
				const auto size = preparedTrace.PreviewSize();
				ImGui::Image(image, { width, width * float(size[1]) / float(std::max(size[0], 1u)) });
				preparedTrace.MarkPreviewDrawSubmitted();
			} else ImGui::TextUnformatted("Waiting for prepared geometry, acceleration and a completed ray dispatch.");
		}
		ImGui::End();
	}
	if (!captureDebug.load()) return;
	CS_GPU_PASS("RedstoneShadows::CaptureDiagnostics");
	const auto* viewport = ImGui::GetMainViewport();
	auto* draw = ImGui::GetForegroundDrawList();
	const auto frame = globals::game::graphicsState->GetFrameCount();
	const bool valid = acquisitionActive && debugScope.batch && debugScope.scene == CasterObservation::Epoch() && frame >= debugScope.frame;
	const auto age = valid ? frame - debugScope.frame : 0;
	constexpr ImU32 kRecentColor = IM_COL32(80, 220, 255, 200);
	constexpr ImU32 kOlderColor = IM_COL32(255, 180, 60, 200);
	if (valid) {
		const auto origin = Util::GetEyePosition(0);
		const auto& camera = Util::GetCameraData(0);
		for (uint32_t i = 0; i < debugBoundCount; ++i) {
			const auto& bound = debugBounds[i];
			std::array<float3, 8> corners;
			for (uint32_t corner = 0; corner < corners.size(); ++corner)
				corners[corner] = { bound.x - origin.x + ((corner & 1) ? bound.w : -bound.w),
					bound.y - origin.y + ((corner & 2) ? bound.w : -bound.w),
					bound.z - origin.z + ((corner & 4) ? bound.w : -bound.w) };
			Util::Region::ScreenBounds screen;
			if (Util::Region::ProjectBounds(camera.viewProjMat, corners, screen) != Util::Region::ProjectionResult::kVisible) continue;
			draw->AddRect({ viewport->Pos.x + screen.minX * viewport->Size.x, viewport->Pos.y + screen.minY * viewport->Size.y },
				{ viewport->Pos.x + screen.maxX * viewport->Size.x, viewport->Pos.y + screen.maxY * viewport->Size.y }, age <= 1 ? kRecentColor : kOlderColor);
		}
	}
	ImGui::SetNextWindowBgAlpha(0.85f);
	if (ImGui::Begin("RedstoneShadows capture###RedstoneShadowsCapture", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
		ImGui::TextUnformatted("Partial local-light capture | native shadows active");
		ImGui::TextUnformatted("Boxes: captured sphere bounds, visible through walls; not triangles.");
		if (!acquisitionActive) ImGui::TextWrapped("Acquisition inactive: %s", unavailable.empty() ? "enable Observe SCM shadow casters" : unavailable.c_str());
		else if (!valid) ImGui::TextUnformatted("Waiting for a captured batch in this scene.");
		if (valid) {
			ImGui::TextUnformatted(std::format("Slot {} | light lifetime {} | batch {} | age {} frames{}", debugScope.slot, debugScope.light, debugScope.batch, age, age > 1 ? " (older capture, amber)" : " (recent, cyan)").c_str());
			ImGui::Text("Captured bounds: %u | incomplete: %s", debugBoundCount, debugStatus.value("incomplete", false) ? "yes" : "no");
			for (const auto& [reason, count] : debugStatus.at("omissions").items())
				ImGui::Text("Omitted in displayed capture (%s): %u", reason.c_str(), count.get<uint32_t>());
		}
		const auto live = GetDiagnostics();
		const auto& copies = live.at("scene");
		const auto& exports = live.at("geometryTransport");
		const auto& as = live.at("acceleration");
		const auto& observation = live.at("observation");
		const auto& events = observation.at("events");
		const auto liveBatch = copies.value("batch", 0ull);
		const char* stage = !acquisitionActive ? "Inactive" : copies.value("copyPending", false) ? "Waiting for D3D11 copy completion" :
			!liveBatch ? "Waiting for a natural SCM observation" : !copies.value("instances", 0ull) ? "Captured batch has no admitted geometry" :
			!workerBatch ? "Preparing DX12 mesh exports" : as.value("workerBusy", false) ? "AS worker running" :
			as.value("batchFinished", false) ? "AS batch finished (check completion/failure below)" : "Waiting for AS worker/GPU progress";
		ImGui::Separator();
		ImGui::Text("LIVE pipeline: %s", stage);
		ImGui::TextUnformatted(std::format("Observation: slot {} | completed {} | dropped {} | selected slot accumulated: {}",
			observation.value("selectedSlot", 0u), observation.value("completed", 0ull), observation.value("dropped", 0ull),
			observation.value("missingObservation", true) ? "no" : "yes").c_str());
		if (ImGui::TreeNode("Capture counters and admission reasons")) {
			ImGui::TextUnformatted("Counters persist across toggles; admission totals cover captures begun with diagnostics enabled.");
			ImGui::TextUnformatted("Callback/admission totals publish at capture end. Rejections show the first failing check.");
			ImGui::Text("Selection calls %llu | selected frames %llu | empty selections %llu",
				events.value("selectCalls", 0ull), events.value("selections", 0ull), events.value("emptySelections", 0ull));
			ImGui::Text("Capture attempts %llu | starts %llu | ends %llu | queue full %llu",
				events.value("beginCalls", 0ull), events.value("captureStarts", 0ull), events.value("captureEnds", 0ull), events.value("queueFull", 0ull));
			ImGui::Text("Observation callbacks: captured %llu | outside capture %llu | after overflow %llu",
				events.value("callbacksInCapture", 0ull), events.value("callbacksOutsideCapture", 0ull), events.value("overflowCallbacks", 0ull));
			ImGui::Text("Admitted %llu | duplicates %llu | epoch invalidations %llu | ended with stale epoch %llu",
				events.value("admitted", 0ull), events.value("duplicates", 0ull), events.value("epochInvalidations", 0ull), events.value("captureEpochMismatch", 0ull));
			ImGui::Text("Admitted with FULLPREC flag clear (SSE float positions): %llu", events.value("admittedWithoutFullPrecisionFlag", 0ull));
			ImGui::TextUnformatted("Callback counts cover SCM-light observation calls, not unrelated scene culling.");
			for (const auto& [reason, count] : events.items()) {
				if (count.get<uint64_t>()) ImGui::TextUnformatted(std::format("{}: {}", reason, count.get<uint64_t>()).c_str());
			}
			if (ImGui::TreeNode("Admission rejections (including zero counts)")) {
				for (const auto& [reason, count] : observation.at("admissionRejections").items())
					ImGui::TextUnformatted(std::format("{}: {}", reason, count.get<uint64_t>()).c_str());
				ImGui::TreePop();
			}
			ImGui::TreePop();
		}
		ImGui::Text("Epoch-discarded batches (session): queued %llu | copying %llu | reset %llu | preparing %llu",
			copies.value("queuedEpochDiscards", 0ull), copies.value("copyEpochDiscards", 0ull), copies.value("resetEpochDiscards", 0ull),
			static_cast<unsigned long long>(preparingEpochDiscards));
		ImGui::TextUnformatted(std::format("Live copy batch {} | instances {} | preparing {} | worker {}",
			liveBatch, copies.value("instances", 0ull), preparingBatch, workerBatch).c_str());
		ImGui::Text("Copy: %s | copied bytes %llu | resident bytes %llu | busy polls %llu",
			copies.value("copyPending", false) ? "pending" : liveBatch ? "complete" : "no batch",
			copies.value("copiedBytes", 0ull), copies.value("residentBytes", 0ull), copies.value("busyFrames", 0ull));
		ImGui::Text("Exports: %llu submitted / %llu complete | ready meshes %llu | prepared %llu",
			exports.value("submittedExports", 0ull), exports.value("completedExports", 0ull), exports.value("residentMeshes", 0ull),
			static_cast<unsigned long long>(preparedGeometry.size()));
		ImGui::Text("Export fence: %llu / %llu (complete / submitted) | busy polls %llu",
			exports.value("completedFence", 0ull), exports.value("submittedFence", 0ull), exports.value("busyFrames", 0ull));
		ImGui::Text("Budget per stage: %llu bytes | export resident: %llu | deferrals: %llu",
			live.value("stageBudgetBytes", 0ull), exports.value("residentBytes", 0ull), exports.value("budgetDeferrals", 0ull));
		const auto deferral = exports.value("lastDeferral", std::string{});
		if (!deferral.empty()) ImGui::TextWrapped("Last export deferral: %s (mesh needs %llu bytes; stage budget %llu)",
			deferral.c_str(), exports.value("lastRequiredBytes", 0ull), exports.value("lastBudgetBytes", 0ull));
		ImGui::Text("AS input: %llu instances / %llu meshes | worker: %s",
			as.value("inputInstances", 0ull), as.value("inputMeshes", 0ull), as.value("workerBusy", false) ? "busy" : "idle");
		if (as.value("countersAvailable", false)) {
			ImGui::Text("Last worker totals: BLAS %llu / %llu | TLAS %llu / %llu (submitted / complete)",
				as.value("bottomSubmissions", 0ull), as.value("bottomCompletions", 0ull), as.value("topSubmissions", 0ull), as.value("topCompletions", 0ull));
			ImGui::TextUnformatted(std::format("Counter sample: scene {} / batch {} | AS resident bytes {} | budget deferrals {}",
				as.value("counterScene", 0ull), as.value("counterBatch", 0ull), as.value("residentBytes", 0ull), as.value("budgetDeferrals", 0ull)).c_str());
		} else ImGui::TextUnformatted("BLAS/TLAS: no worker counter sample yet (not a measured zero).");
		ImGui::Text("AS fence: %llu / %llu (complete / submitted) | matches active batch: %s",
			as.value("completedFence", 0ull), as.value("submittedFence", 0ull), as.value("publicationMatchesBatch", false) ? "yes" : "no");
		ImGui::TextUnformatted(std::format("Last completed TLAS: scene {} / batch {} / light lifetime {}",
			as.value("lastCompletedScene", 0ull), as.value("lastCompletedBatch", 0ull), as.value("lastCompletedLight", 0ull)).c_str());
		ImGui::TextUnformatted(std::format("Discarded batches {} | last {}: {}", discardedBatches, lastDiscardBatch,
			lastDiscardReason.empty() ? "none" : lastDiscardReason).c_str());
		const auto lastASFailure = as.value("lastBatchFailure", std::string{});
		if (!lastASFailure.empty()) ImGui::TextWrapped("Last AS failure (batch %llu): %s", as.value("lastFailedBatch", 0ull), lastASFailure.c_str());
		for (const auto* stageStatus : { &copies, &exports, &as }) {
			const auto failure = stageStatus->value("failure", std::string{});
			if (!failure.empty()) ImGui::TextWrapped("Failure: %s", failure.c_str());
		}
		ImGui::TextUnformatted("Diagnostic snapshot only. No tracing, lighting replacement or CPU bypass.");
	}
	ImGui::End();
}

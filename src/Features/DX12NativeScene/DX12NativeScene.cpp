#include "DX12NativeScene.h"

#include "Globals.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/DevBenchUx.h"
#include "Utils/Region.h"
#include "Util.h"
#include <chrono>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DX12NativeScene::Settings, AcquireScene, UsePreparedScene, PreviewPreparedScene, UsePreparedMaterials, PreparedMaterialView, MaterialTextureBudgetMB, DirectLightingBudgetMB, PreviewLight, PreviewAllLights, PreviewDirectStrength, PreviewDiffuseStrength, PreviewSpecularStrength, PreviewEmissionStrength, NodesPerFrame, MaxInstances, BufferBudgetMB)

std::pair<std::string, std::vector<std::string>> DX12NativeScene::GetFeatureSummary()
{
	return { "Native DX12 scene preparation, materials, acceleration structures and SCM observation diagnostics.", {} };
}
void DX12NativeScene::ImportLegacySettings(json& root)
{
	if (root.contains(GetName())) return;
	auto it = root.find("RedstoneShadows");
	if (it == root.end() || !it->is_object()) return;
	json migrated = json::object();
	for (const char* field : { "AcquireScene", "UsePreparedScene", "PreviewPreparedScene", "UsePreparedMaterials",
			"PreparedMaterialView", "MaterialTextureBudgetMB", "DirectLightingBudgetMB", "PreviewLight", "PreviewAllLights",
			"PreviewDirectStrength", "PreviewDiffuseStrength", "PreviewSpecularStrength", "PreviewEmissionStrength",
			"NodesPerFrame", "MaxInstances", "BufferBudgetMB" })
		if (it->contains(field)) migrated[field] = (*it)[field];
	if (!migrated.contains("PreviewLight") && it->contains("PreparedLight")) migrated["PreviewLight"] = (*it)["PreparedLight"];
	if (!migrated.contains("PreviewAllLights") && it->contains("UseAllPreparedLights")) migrated["PreviewAllLights"] = (*it)["UseAllPreparedLights"];
	for (const auto& p : { std::pair{"PreviewDirectStrength", "DirectStrength"}, {"PreviewDiffuseStrength", "DiffuseStrength"},
			{"PreviewSpecularStrength", "SpecularStrength"}, {"PreviewEmissionStrength", "EmissionStrength"} })
		if (!migrated.contains(p.first) && it->contains(p.second)) migrated[p.first] = (*it)[p.second];
	if (it->value("UseDXRIndirect", false)) migrated["UsePreparedScene"] = migrated["UsePreparedMaterials"] = true;
	root[GetName()] = std::move(migrated);
}
void DX12NativeScene::LoadSettings(json& value)
{
	Settings selected = value;
	selected.NodesPerFrame = std::clamp(selected.NodesPerFrame, 64u, 8192u);
	selected.MaxInstances = std::clamp(selected.MaxInstances, 1u, CasterObservation::kCapacity);
	selected.BufferBudgetMB = std::clamp(selected.BufferBudgetMB, 16u, 512u);
	selected.PreviewLight = std::min(selected.PreviewLight, 255u);
	selected.PreparedMaterialView = std::min(selected.PreparedMaterialView, RedstoneShadow::DirectionalTrace::kCombinedDirectView);
	selected.MaterialTextureBudgetMB = std::clamp(selected.MaterialTextureBudgetMB, 64u, 1024u);
	selected.DirectLightingBudgetMB = std::clamp(selected.DirectLightingBudgetMB, 64u, 1024u);
	for (auto* v : { &selected.PreviewDirectStrength, &selected.PreviewDiffuseStrength,
			&selected.PreviewSpecularStrength, &selected.PreviewEmissionStrength })
		*v = std::isfinite(*v) ? std::clamp(*v, 0.0f, 8.0f) : 1.0f;
	if (selected.UsePreparedScene != settings.UsePreparedScene) ready = false;
	settings = selected;
	resetRequested = true;
}
void DX12NativeScene::SaveSettings(json& value) { value = settings; }
void DX12NativeScene::RestoreDefaultSettings()
{
	settings = {};
	ready = false;
	resetRequested = true;
}

void DX12NativeScene::SetupResources()
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

void DX12NativeScene::EarlyPrepass()
{
	CS_GPU_PASS("DX12NativeScene::AcquireScene");
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

void DX12NativeScene::AdvanceAcquisition(const GraphicsBackend& a_backend, uint64_t a_frame, uint64_t a_budgetBytes)
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

void DX12NativeScene::OnSceneTransitionReset(bool a_open)
{
	transitionOpen = a_open;
	resetRequested = true;
}

void DX12NativeScene::OnRuntimeDisabled()
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

json DX12NativeScene::GetDiagnostics()
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
		{ "sceneCompositeSubmitted", traceStatus.value("sceneCompositeSubmissions", 0ull) != 0 },
		{ "cpuProductionBypassed", false }, { "fallbackReason", "Native shadow production remains active; optional selected-light direct scene mode replaces only supported depth-matched opaque pixels" }
	};
}

void DX12NativeScene::DrawSettings() { DrawNativeSceneSettings(); }

void DX12NativeScene::DrawNativeSceneSettings()
{
    bool useNativeScene = settings.UsePreparedScene && settings.UsePreparedMaterials;
    if (ImGui::Checkbox("Use DX12 native scene", &useNativeScene)) {
        if (settings.UsePreparedScene != useNativeScene) ready = false;
        settings.UsePreparedScene = useNativeScene;
        settings.UsePreparedMaterials = useNativeScene;
        resetRequested = true;
    }
    if (settings.UsePreparedScene != settings.UsePreparedMaterials)
        ImGui::TextWrapped("Legacy settings enable only part of the native scene. Changing the unified toggle will enable or disable geometry and materials together.");

    ImGui::SeparatorText("Budgets");
    int instances = int(settings.MaxInstances), geometryBudget = int(settings.BufferBudgetMB);
    bool geometryChanged = ImGui::SliderInt("Maximum admitted instances", &instances, 1, int(CasterObservation::kCapacity));
    geometryChanged |= ImGui::SliderInt("Geometry buffer budget (MiB)", &geometryBudget, 16, 512);
    if (geometryChanged) {
        settings.MaxInstances = uint32_t(instances);
        settings.BufferBudgetMB = uint32_t(geometryBudget);
        resetRequested = true;
    }
    int materialBudget = int(settings.MaterialTextureBudgetMB);
    if (ImGui::SliderInt("Material texture budget (MiB)", &materialBudget, 64, 1024)) {
        settings.MaterialTextureBudgetMB = uint32_t(materialBudget);
        resetRequested = true;
    }
    int lightingBudget = int(settings.DirectLightingBudgetMB);
    if (ImGui::SliderInt("Prepared lighting buffer budget (MiB)", &lightingBudget, 64, 1024))
        settings.DirectLightingBudgetMB = uint32_t(lightingBudget);
    ImGui::TextWrapped("The prepared lighting budget also covers reconstruction buffers; insufficient capacity falls back to raw signals. Provider-internal allocations are additional.");

    if (ImGui::Checkbox("Observe SCM shadow casters", &settings.AcquireScene)) resetRequested = true;
    ImGui::TextWrapped("Observes one SCM local-light slot when updated. Native shadows remain active.");

    ImGui::SeparatorText("Debug");
    if (ImGui::Checkbox("Native scene preview", &settings.PreviewPreparedScene)) preparedTrace.Reset();
    if (settings.UsePreparedMaterials) {
        int view = int(settings.PreparedMaterialView);
        if (ImGui::Combo("Prepared material view", &view, "Visibility\0Base color\0World normal\0Source roughness\0Metalness\0Emission\0Material coverage\0Material AO\0PBR F0\0Opacity\0Direct diffuse\0Direct specular\0Direct combined and emission\0")) {
            settings.PreparedMaterialView = uint32_t(view);
            preparedTrace.Reset();
        }
        const auto prepared = preparedScene.GetDiagnostics();
        int previewLight = int(settings.PreviewLight);
        const auto maxPreviewLight = std::max(0, int(prepared.value("lightCount", 0ull)) - 1);
        if (ImGui::SliderInt("Preview baseline light", &previewLight, 0, maxPreviewLight)) {
            settings.PreviewLight = uint32_t(previewLight);
            preparedTrace.Reset();
        }
        if (ImGui::Checkbox("All prepared lights (preview only)", &settings.PreviewAllLights)) preparedTrace.Reset();
        if (ImGui::TreeNode("Preview lighting strengths")) {
            ImGui::SliderFloat("Direct##nativePreview", &settings.PreviewDirectStrength, 0.0f, 8.0f, "%.2fx");
            ImGui::SliderFloat("Diffuse##nativePreview", &settings.PreviewDiffuseStrength, 0.0f, 8.0f, "%.2fx");
            ImGui::SliderFloat("Specular##nativePreview", &settings.PreviewSpecularStrength, 0.0f, 8.0f, "%.2fx");
            ImGui::SliderFloat("Emission##nativePreview", &settings.PreviewEmissionStrength, 0.0f, 8.0f, "%.2fx");
            ImGui::TreePop();
        }
        ImGui::TextWrapped("Material views trace prepared geometry; orange marks unsupported techniques and magenta unavailable data. These are source-material views, not final scene lighting.");
        const auto trace = preparedTrace.GetDiagnostics();
        ImGui::Text("Texture pools: %llu | unique textures: %u | texture MiB: %.1f", trace.value("texturePools", 0ull), trace.value("uniqueTextures", 0u), double(trace.value("materialTextureBytes", 0ull) / (1024.0 * 1024.0)));
        ImGui::Text("Ready source materials: %u | rejected: %u", trace.value("readySourceMaterials", 0u), trace.value("rejectedSourceMaterials", 0u));
        if (!trace.value("materialFailure", std::string{}).empty()) ImGui::TextWrapped("%s", trace.at("materialFailure").get_ref<const std::string&>().c_str());
    }
    bool showCaptureDebug = captureDebug.load();
    if (ImGui::Checkbox("Capture diagnostics (temporary overlay)", &showCaptureDebug)) {
        captureDebug.store(showCaptureDebug);
        CasterObservation::SetDiagnosticsEnabled(showCaptureDebug);
        debugBoundCount = 0;
        debugScope = {};
        debugStatus = {};
    }
    ImGui::TextWrapped("Diagnostic boxes enclose captured world bounds, including hidden objects. They are not mesh wireframes or ray-traced shadows.");
    const auto prepared = preparedScene.GetDiagnostics();
    ImGui::Text("Prepared meshes: %llu | instances: %llu | material-masked: %u", prepared.value("meshes", 0ull), prepared.value("instances", 0ull), prepared.value("maskedInstances", 0u));
    if (prepared.contains("failure") && !prepared.at("failure").get_ref<const std::string&>().empty())
        ImGui::TextWrapped("%s", prepared.at("failure").get_ref<const std::string&>().c_str());
    if (!unavailable.empty()) ImGui::TextWrapped("%s", unavailable.c_str());
    ImGui::Text("DXR: %s; RR session: %s", dxrCapable ? "supported" : "unavailable", rrEligible ? "eligible" : "ineligible");
}

void DX12NativeScene::RegisterUxActions()
{
	FEATURE_COMMAND("setPreparedMaterials", "Configure prepared material maps and diagnostic views.",
		[](Feature* self, const json& args) {
			auto& n = *static_cast<DX12NativeScene*>(self);
			const bool previousEnabled = n.settings.UsePreparedMaterials;
			const uint32_t previousBudget = n.settings.MaterialTextureBudgetMB;
			n.settings.UsePreparedMaterials = args.at("enabled").get<bool>();
			if (args.contains("view")) n.settings.PreparedMaterialView = std::clamp(args.at("view").get<uint32_t>(), 0u, RedstoneShadow::DirectionalTrace::kCombinedDirectView);
			if (args.contains("textureBudgetMB")) n.settings.MaterialTextureBudgetMB = std::clamp(args.at("textureBudgetMB").get<uint32_t>(), 64u, 1024u);
			if (args.contains("allLights")) n.settings.PreviewAllLights = args.at("allLights").get<bool>();
			if (previousEnabled != n.settings.UsePreparedMaterials || previousBudget != n.settings.MaterialTextureBudgetMB) n.resetRequested = true;
			else n.preparedTrace.Reset();
		});
	FEATURE_COMMAND("setPreparedScene", "Enable prepared geometry and diagnostic preview.",
		[](Feature* self, const json& args) {
			auto& n = *static_cast<DX12NativeScene*>(self);
			n.settings.UsePreparedScene = args.at("enabled").get<bool>();
			if (args.contains("preview")) n.settings.PreviewPreparedScene = args.at("preview").get<bool>();
			if (args.contains("light")) n.settings.PreviewLight = std::clamp(args.at("light").get<uint32_t>(), 0u, 255u);
			n.ready = false; n.resetRequested = true;
		});
	FEATURE_COMMAND("setSceneAcquisition", "Toggle SCM local-light caster observation.",
		[](Feature* self, const json& args) {
			auto& n = *static_cast<DX12NativeScene*>(self);
			n.settings.AcquireScene = args.at("enabled").get<bool>();
			n.resetRequested = true;
		});
	FEATURE_COMMAND("resetShadowScene", "Invalidate scene data at next render boundary.",
		[](Feature* self, const json&) { static_cast<DX12NativeScene*>(self)->resetRequested = true; });
	FEATURE_QUERY("shadowStatus", "Return native scene and SCM diagnostics.",
		[](const Feature* self, const json&) { return const_cast<DX12NativeScene*>(static_cast<const DX12NativeScene*>(self))->GetDiagnostics(); });
}

json DX12NativeScene::GetRuntimeFlags()
{
	return { { "CaptureDebug", captureDebug.load() }, { "TraceDirectional", false }, { "PreviewDirectional", false } };
}

bool DX12NativeScene::SetRuntimeFlag(std::string_view a_name, bool a_value)
{
	if (a_name == "CaptureDebug") {
		captureDebug.store(a_value);
		CasterObservation::SetDiagnosticsEnabled(a_value);
		return true;
	}
	return (a_name == "TraceDirectional" || a_name == "PreviewDirectional") && !a_value;
}

bool DX12NativeScene::IsOverlayVisible() const
{
	return (captureDebug.load() || (settings.UsePreparedScene && settings.PreviewPreparedScene)) && !globals::game::isVR;
}

void DX12NativeScene::UpdateCaptureDebug()
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

void DX12NativeScene::DrawOverlay()
{
	if (!IsOverlayVisible()) return;
	if (settings.UsePreparedScene && settings.PreviewPreparedScene) {
		CS_GPU_PASS("DX12NativeScene::PreparedPreviewDraw");
		if (ImGui::Begin("Prepared cell shadow preview###RedstonePreparedPreview")) {
			static constexpr std::array views{ "Visibility", "Source base color", "World normal", "Source roughness", "Metalness", "Emission", "Material coverage", "Material AO", "PBR F0", "Opacity", "Direct diffuse", "Direct specular", "Direct combined and emission" };
			const auto selectedView = settings.UsePreparedMaterials ? std::min(settings.PreparedMaterialView, RedstoneShadow::DirectionalTrace::kCombinedDirectView) : 0u;
			const auto trace = preparedTrace.GetDiagnostics();
			ImGui::Text("%s | native shadows active", views[selectedView]);
			if (settings.PreviewAllLights && selectedView >= RedstoneShadow::DirectionalTrace::kFirstDirectView)
				ImGui::Text("All prepared lights | submitted light count: %u | limit: %u", trace.value("requestedLightCount", 0u), RedstoneShadow::DirectionalTrace::kMaximumDirectLights);

			else if (selectedView >= RedstoneShadow::DirectionalTrace::kFirstDirectView) ImGui::TextUnformatted("Selected-light direct radiance; display tone map. Black can be valid zero lighting.");
			ImGui::TextWrapped("Red: no depth receiver | blue: no ray hit | orange: unsupported material | magenta: no light match/material unavailable | yellow: depth mismatch | white: invalid data. Cyan: unknown room | gray: room excludes light.");
			if (settings.UsePreparedMaterials) {
				if (trace.value("bindingsLoading", false)) ImGui::TextUnformatted("Preparing material tables and texture uploads...");
				else ImGui::Text("Texture pools: %llu | textures: %u | source materials ready: %u | unsupported/unavailable: %u",
					trace.value("texturePools", 0ull), trace.value("uniqueTextures", 0u), trace.value("readySourceMaterials", 0u), trace.value("rejectedSourceMaterials", 0u));
				const auto materialFailure = trace.value("materialFailure", std::string{});
				if (!materialFailure.empty()) ImGui::TextWrapped("Material load failed: %s", materialFailure.c_str());
				ImGui::TextUnformatted("Magenta: unavailable data | orange: unsupported technique. Source channels; runtime compatibility unverified.");
			} else ImGui::TextUnformatted("Prepared material maps are disabled.");
			ImGui::TextWrapped("Room rejection: cyan = partial coverage without code; pale cyan = partial coverage with code; dark cyan = zero coverage and code (unwritten or invalidated); green = zero code at full coverage; lime = invalid code; royal blue = generation mismatch; gold = missing generation; teal = no metadata target. Light status: dark purple = no live table; brown = unresolved reference; pink = ambiguous identity; violet = unsupported light type. Light match is exact reference/base identity, not position.");
			const auto selectionFailure = trace.value("lightSelectionFailure", std::string{});
			if (!selectionFailure.empty()) ImGui::TextWrapped("Light set unavailable: %s", selectionFailure.c_str());
			const auto traceFailure = trace.value("failure", std::string{});
			if (!traceFailure.empty()) ImGui::TextWrapped("Ray dispatch failed: %s", traceFailure.c_str());
			if (auto* image = preparedTrace.Preview()) {
				const auto width = std::max(ImGui::GetContentRegionAvail().x, 128.0f);
				const auto size = preparedTrace.PreviewSize();
				ImGui::Image(image, { width, width * float(size[1]) / float(std::max(size[0], 1u)) });
				preparedTrace.MarkPreviewDrawSubmitted();
			} else if (traceFailure.empty() && selectionFailure.empty()) ImGui::TextUnformatted("Waiting for prepared geometry, acceleration and a completed ray dispatch.");
		}
		ImGui::End();
	}
	if (!captureDebug.load()) return;
	CS_GPU_PASS("DX12NativeScene::CaptureDiagnostics");
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
	if (ImGui::Begin("DX12 Native Scene capture###DX12NativeSceneCapture", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
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


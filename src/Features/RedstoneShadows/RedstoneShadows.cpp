#include "RedstoneShadows.h"

#include "Globals.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/DevBenchUx.h"
#include "Utils/Region.h"
#include "Util.h"
#include <chrono>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RedstoneShadows::Settings, AcquireScene, UsePreparedScene, PreviewPreparedScene, UsePreparedMaterials, UsePreparedDirectLighting, UseAllPreparedLights, ConnectIndirectLighting, UseDirectRayRegeneration, UseDXRIndirect, IndirectRays, IndirectStrength, RedstoneBlend, DirectStrength, DiffuseStrength, SpecularStrength, EmissionStrength, PreparedMaterialView, MaterialTextureBudgetMB, DirectLightingBudgetMB, PreparedLight, NodesPerFrame, MaxInstances, BufferBudgetMB)

std::pair<std::string, std::vector<std::string>> RedstoneShadows::GetFeatureSummary()
{
	return { "Prepared cell geometry feeds native DX12 acceleration, material previews and optional prepared-light RT diffuse/specular scene lighting. SCM observation remains separate. Native shadow production remains active.", {} };
}

void RedstoneShadows::LoadSettings(json& a_json)
{
	Settings selected = a_json;
	selected.IndirectRays = std::clamp(selected.IndirectRays, 1u, 8u);
	if (selected.UseDXRIndirect) selected.UsePreparedScene = selected.UsePreparedMaterials = true;
	selected.NodesPerFrame = std::clamp(selected.NodesPerFrame, 64u, 8192u);
	selected.MaxInstances = std::clamp(selected.MaxInstances, 1u, CasterObservation::kCapacity);
	selected.BufferBudgetMB = std::clamp(selected.BufferBudgetMB, 16u, 512u);
	selected.PreparedLight = std::min(selected.PreparedLight, 255u);
	selected.PreparedMaterialView = std::min(selected.PreparedMaterialView, RedstoneShadow::DirectionalTrace::kCombinedDirectView);
	selected.MaterialTextureBudgetMB = std::clamp(selected.MaterialTextureBudgetMB, 64u, 1024u);
	selected.DirectLightingBudgetMB = std::clamp(selected.DirectLightingBudgetMB, 64u, 1024u);
	selected.RedstoneBlend = std::isfinite(selected.RedstoneBlend) ? std::clamp(selected.RedstoneBlend, 0.0f, 1.0f) : 1.0f;
	if (selected.UsePreparedScene != settings.UsePreparedScene) ready = false;
	for (auto* value : { &selected.DirectStrength, &selected.DiffuseStrength, &selected.SpecularStrength, &selected.EmissionStrength, &selected.IndirectStrength })
		*value = std::isfinite(*value) ? std::clamp(*value, 0.0f, 8.0f) : 1.0f;
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
		{ "preparedDirectLightingEnabled", settings.UsePreparedDirectLighting }, { "connectIndirectLighting", settings.ConnectIndirectLighting }, { "useDirectRayRegeneration", settings.UseDirectRayRegeneration }, { "indirectStrength", settings.IndirectStrength }, { "rgiSlices", globals::features::redstoneGI.GetLightingSamples() }, { "indirectProducer", settings.UseDXRIndirect ? "DXR" : "raymarched" }, { "indirectRays", settings.IndirectRays }, { "allPreparedLightsEnabled", settings.UseAllPreparedLights }, { "directLightingBudgetBytes", uint64_t(settings.DirectLightingBudgetMB) * 1024 * 1024 },
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

void RedstoneShadows::DrawSettings()
{
	if (ImGui::Checkbox("Use prepared cell geometry", &settings.UsePreparedScene)) { ready = false; resetRequested = true; }
	if (ImGui::Checkbox("Prepared local-light shadow preview", &settings.PreviewPreparedScene)) { preparedTrace.Reset(); }
	if (ImGui::Checkbox("Load prepared material maps", &settings.UsePreparedMaterials)) resetRequested = true;
	if (settings.UsePreparedMaterials) {
		int view = int(settings.PreparedMaterialView), materialBudget = int(settings.MaterialTextureBudgetMB);
		if (ImGui::Combo("Prepared material view", &view, "Visibility\0Base color\0World normal\0Source roughness\0Metalness\0Emission\0Material coverage\0Material AO\0PBR F0\0Opacity\0Direct diffuse\0Direct specular\0Direct combined and emission\0")) { settings.PreparedMaterialView = uint32_t(view); preparedTrace.Reset(); }
		if (ImGui::SliderInt("Material texture budget (MiB)", &materialBudget, 64, 1024)) { settings.MaterialTextureBudgetMB = uint32_t(materialBudget); resetRequested = true; }
		ImGui::TextWrapped("Prepared maps load once on a background worker. Material views trace the prepared geometry; orange marks unsupported techniques and magenta marks unavailable data. These are baseline source-material views, not final game lighting.");
		const auto trace = preparedTrace.GetDiagnostics();
		ImGui::Text("Texture pools: %llu | unique textures: %u | texture MiB: %.1f", trace.value("texturePools", 0ull), trace.value("uniqueTextures", 0u), double(trace.value("materialTextureBytes", 0ull)) / (1024.0 * 1024.0));
		ImGui::Text("Ready source materials: %u | rejected: %u", trace.value("readySourceMaterials", 0u), trace.value("rejectedSourceMaterials", 0u));
		if (!trace.value("materialFailure", std::string{}).empty()) ImGui::TextWrapped("%s", trace.at("materialFailure").get_ref<const std::string&>().c_str());
	}
	ImGui::SliderFloat("Raster / Redstone blend", &settings.RedstoneBlend, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat("Direct lighting strength", &settings.DirectStrength, 0.0f, 8.0f, "%.2fx");
	ImGui::SliderFloat("Direct diffuse strength", &settings.DiffuseStrength, 0.0f, 8.0f, "%.2fx");
	ImGui::SliderFloat("Direct specular strength", &settings.SpecularStrength, 0.0f, 8.0f, "%.2fx");
	ImGui::SliderFloat("Prepared emission strength", &settings.EmissionStrength, 0.0f, 8.0f, "%.2fx");
	if (ImGui::Button("Reset lighting strengths")) settings.DirectStrength = settings.DiffuseStrength = settings.SpecularStrength = settings.EmissionStrength = settings.IndirectStrength = 1.0f;
	ImGui::TextWrapped("Strength changes apply to the next direct-light frame or preview dispatch. Direct strength scales diffuse and specular; emission is independent. Normal Save persists values.");
	if (ImGui::Checkbox("All prepared cell lights (direct only)", &settings.UseAllPreparedLights)) preparedTrace.Reset();
	if (ImGui::Checkbox("Prepared RT direct scene lighting", &settings.UsePreparedDirectLighting)) preparedTrace.Reset();
	if (settings.UsePreparedDirectLighting || settings.UseDXRIndirect) {
		ImGui::TextWrapped("Requires prepared geometry and material maps. Replaces supported, depth-matched opaque pixels with direct diffuse/specular plus emission. All-prepared mode accumulates every prepared light, up to 32; selected mode uses the baseline selector. Missing or unsupported requested lights keep native output rather than dropping contributions. Unlisted direct lights remain absent. The exclusive indirect selection adds either connected current RedstoneGI output or one-bounce DXR diffuse/specular. DXR requires regenerated baked room data and contributes only validated samples. Runs every frame at native render resolution; native shadow work remains active.");
		const auto trace = preparedTrace.GetDiagnostics();
		if (settings.UseDXRIndirect && trace.value("cachedSceneBindings", false) && !trace.value("bakedRoomsKnown", false))
			ImGui::TextWrapped("DXR indirect requires regenerated room metadata. Rebuild this cell's geometry cache with the current preparation tool.");
		ImGui::Text("Submitted light count: %u | limit: %u", trace.value("requestedLightCount", 0u), RedstoneShadow::DirectionalTrace::kMaximumDirectLights);
		ImGui::Text("Scene submissions: %llu | last frame: %u | budget deferrals: %llu | busy frames: %llu",
			trace.value("sceneCompositeSubmissions", 0ull), trace.value("sceneCompositeFrame", 0u), trace.value("budgetDeferrals", 0ull), trace.value("busyFrames", 0ull));
		const auto failure = trace.value("failure", std::string{});
		if (!failure.empty()) ImGui::TextWrapped("Direct lighting unavailable: %s", failure.c_str());
	}
	const auto selectionFailure = preparedTrace.GetDiagnostics().value("lightSelectionFailure", std::string{});
	if (!selectionFailure.empty()) ImGui::TextWrapped("Light set unavailable: %s", selectionFailure.c_str());
	const auto prepared = preparedScene.GetDiagnostics();
	int light = int(settings.PreparedLight);
	const auto maximumLight = std::max(0, int(prepared.value("lightCount", 0ull)) - 1);
	if (ImGui::SliderInt("Prepared baseline light", &light, 0, maximumLight)) { settings.PreparedLight = uint32_t(light); preparedTrace.Reset(); }
	ImGui::TextWrapped("Visibility and direct-light views require one evaluated point light with the selected placed-reference and base identity; flicker movement does not affect matching. PortalStrict uses the current native receiver room mask. Source-material views inspect prepared world-space ray hits without a live light. Magenta marks unavailable data; orange marks unsupported material techniques. Native shadows remain active; saved/moved objects and animation are not validated.");
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
	FEATURE_COMMAND("setPreparedRRDebug",
		"Select temporary prepared RR debug views. Required source: off, inputs or outputs. Optional signal integer 0..6: direct diffuse, direct specular, direct combined, indirect diffuse, indirect specular, indirect combined, primary validity (default 0); buffer integer 0..15 selects the inline buffer (default 2); exposure finite -8..8 EV (default 0). Full-screen selection is exclusive with raymarched RR debug. Does not change ray tracing, reconstruction or saved settings. Previews force display alpha 1, expand checkerboard inputs, and label logical RR dimensions separately from packed storage/context capacity. Rejected primary pixels are magenta/yellow, unavailable indirect/output orange; valid black radiance remains black.",
		[](Feature* self, const json& args) {
			const auto source = args.at("source").get<std::string>();
			if (source != "off" && source != "inputs" && source != "outputs") throw std::invalid_argument("Source must be off, inputs or outputs");
			auto* shadows = static_cast<RedstoneShadows*>(self);
			shadows->preparedTrace.SetRRDebug(source == "inputs" ? 1u : source == "outputs" ? 2u : 0u, args.value("signal", 0u), args.value("buffer", 2u), args.value("exposure", 0.0f));
			if (source != "off") {
				globals::features::redstoneGI.SetRROutputOnly(false);
				globals::features::redstoneGI.SetRRInputsOnly(false);
			}
		});
	FEATURE_COMMAND("setIndirectProducer",
		"Select exclusive indirect lighting. Required producer: raymarched or dxr. Optional rays integer 1..8 controls normalized one-bounce DXR hemisphere samples per native pixel; each sample can add shadow rays per prepared light. DXR requires regenerated roomSchemaVersion 1 geometry, prepared materials, and current native light/room admission. It claims the indirect provider without falling back to raymarching when unavailable. RR grid/checkerboarding are configured independently in Upscaling.setRayRegeneration. Normal Save persists selection and rays.",
		[](Feature* self, const json& args) {
			auto* shadows = static_cast<RedstoneShadows*>(self);
			const auto producer = args.at("producer").get<std::string>();
			if (producer != "dxr" && producer != "raymarched") throw std::invalid_argument("Producer must be dxr or raymarched");
			shadows->settings.UseDXRIndirect = producer == "dxr";
			shadows->settings.IndirectRays = std::clamp(args.value("rays", shadows->settings.IndirectRays), 1u, 8u);
			if (shadows->settings.UseDXRIndirect) {
				shadows->settings.UsePreparedScene = shadows->settings.UsePreparedMaterials = true;
				shadows->resetRequested = true;
			} else globals::features::redstoneGI.SelectRenderer(true);
			shadows->preparedTrace.Reset();
		});
	FEATURE_COMMAND("setLightingSampling",
		"Set existing RedstoneGI screen-space sampling work. Optional diffuseSlices/specularSlices integers 1..10; each slice traces both directions. Values are normalized, do not change expected brightness, reset RGI history, and persist with normal Save. Does not add DXR rays or implement RT indirect lighting.",
		[](Feature*, const json& args) {
			auto& gi = globals::features::redstoneGI;
			const auto current = gi.GetLightingSamples();
			gi.SetLightingSamples(args.value("diffuseSlices", current[0]), args.value("specularSlices", current[1]));
		});
	FEATURE_COMMAND("setLightingStrengths",
		"Adjust live prepared direct lighting. Optional numeric params direct, diffuse, specular, emission, indirect, each finite 0..8; defaults 1. Direct multiplies diffuse/specular only. Applies next direct frame or preview dispatch without resource reset; direct RR history resets on direct strength changes; normal Save persists. Does not change visibility or source-material views.",
		[](Feature* a_self, const json& args) {
			auto& selected = static_cast<RedstoneShadows*>(a_self)->settings;
			for (const auto* name : { "direct", "diffuse", "specular", "emission", "indirect" }) {
				if (args.contains(name) && !std::isfinite(args.at(name).get<float>())) throw std::invalid_argument("Lighting strengths must be finite");
			}
			auto apply = [&](const char* name, float& value) {
				if (args.contains(name)) value = std::clamp(args.at(name).get<float>(), 0.0f, 8.0f);
			};
			apply("direct", selected.DirectStrength);
			apply("diffuse", selected.DiffuseStrength);
			apply("specular", selected.SpecularStrength);
			apply("emission", selected.EmissionStrength);
			apply("indirect", selected.IndirectStrength);
		});

	FEATURE_COMMAND("setPreparedMaterials",
		"Configure prepared texture pools and ray-hit views. Params: enabled (required boolean), view (optional integer 0..12: visibility, base color, world normal, source roughness, metalness, emission, coverage, AO, PBR F0, opacity, direct diffuse, direct specular, direct combined plus emission), textureBudgetMB (optional integer 64..1024), allLights (optional boolean, default false; direct views only, sums 1..32 prepared lights). Preview requires UsePreparedScene and PreviewPreparedScene; direct views require supported PBR material and a unique supported live match for every requested light. View changes reuse immutable bindings. Preview radiance is tone-mapped for display. Normal Save persists selections.",
		[](Feature* a_self, const json& a_args) {
			auto* shadows = static_cast<RedstoneShadows*>(a_self);
			if (a_args.contains("allLights")) shadows->settings.UseAllPreparedLights = a_args.at("allLights").get<bool>();
			const auto wasEnabled = shadows->settings.UsePreparedMaterials;
			const auto previousBudget = shadows->settings.MaterialTextureBudgetMB;
			shadows->settings.UsePreparedMaterials = a_args.at("enabled").get<bool>();
			if (a_args.contains("view")) shadows->settings.PreparedMaterialView = std::clamp(a_args.at("view").get<uint32_t>(), 0u, RedstoneShadow::DirectionalTrace::kCombinedDirectView);
			if (a_args.contains("textureBudgetMB")) shadows->settings.MaterialTextureBudgetMB = std::clamp(a_args.at("textureBudgetMB").get<uint32_t>(), 64u, 1024u);
			if (wasEnabled != shadows->settings.UsePreparedMaterials || previousBudget != shadows->settings.MaterialTextureBudgetMB) shadows->resetRequested = true;
			else shadows->preparedTrace.Reset();
		});
	FEATURE_COMMAND("setPreparedDirectLighting",
		"Enable prepared-point-light RT scene lighting. Params: enabled (required boolean), bufferBudgetMB (optional integer 64..1024, default 256), allLights (optional boolean; false selects one baseline light, true requires 1..32 prepared lights). Enabling also selects prepared geometry and material maps. Each current frame traces native-resolution supported PBR surfaces, requires agreement with opaque depth, evaluates diffuse/specular and emission, and replaces accepted scene pixels before forward transparency. Unavailable/unsupported pixels keep native lighting; replaced pixels omit unlisted direct lights. Optional indirect (default true) connects current selected RedstoneGI output; optional rr (default true) denoises separated RT direct diffuse/specular in an independent context, with raw fallback on provider/budget failure. All-prepared mode sums diffuse/specular per live light and emission once; missing, unsupported or ambiguous requested lights reject the entire pixel, without partial accumulation. GPU queue dependencies retain resources without CPU waits. Does not bypass native shadow production or retire RGI. RGI retains its own indirect RR context. Normal Save persists selections.",
		[](Feature* a_self, const json& a_args) {
			auto* shadows = static_cast<RedstoneShadows*>(a_self);
			const bool enabled = a_args.at("enabled").get<bool>();
			if (enabled && !shadows->settings.UsePreparedScene) { shadows->settings.UsePreparedScene = true; shadows->ready = false; shadows->resetRequested = true; }
			if (enabled && !shadows->settings.UsePreparedMaterials) { shadows->settings.UsePreparedMaterials = true; shadows->resetRequested = true; }
			shadows->settings.UsePreparedDirectLighting = enabled;
			if (a_args.contains("allLights")) shadows->settings.UseAllPreparedLights = a_args.at("allLights").get<bool>();
			if (a_args.contains("indirect")) shadows->settings.ConnectIndirectLighting = a_args.at("indirect").get<bool>();
			if (a_args.contains("rr")) shadows->settings.UseDirectRayRegeneration = a_args.at("rr").get<bool>();
			if (a_args.contains("bufferBudgetMB")) shadows->settings.DirectLightingBudgetMB = std::clamp(a_args.at("bufferBudgetMB").get<uint32_t>(), 64u, 1024u);
			shadows->preparedTrace.Reset();
		});
	FEATURE_QUERY("shadowStatus",
		"Return SCM and prepared loading/AS counters, evaluated light frame/count, texture/material readiness, failures, active view, directSubmissions, sceneCompositeSubmissions and sceneCompositeFrame. Submission counters are historical GPU command evidence; they do not prove accepted pixel coverage or visual correctness. Matching uses exact placed-reference/base IDs. allPreparedLights/requestedLightCount/maximumDirectLights/lightSelectionFailure report the bounded light set. sourceReference/sourceBase, sourceRoomGeneration and sourceRoomMetadata describe submitted inputs, not GPU admission results. GPU reason colors distinguish table/reference/match/type/room failures without CPU readback. Room colors: cyan partial coverage without code, pale cyan partial coverage with code, dark cyan zero coverage/code, green zero code at full coverage, lime invalid code, royal blue stale generation, gold missing generation, teal no target. Full coverage, RR and native CPU bypass remain unvalidated. No params.",
		[](const Feature* a_self, const json&) { return const_cast<RedstoneShadows*>(static_cast<const RedstoneShadows*>(a_self))->GetDiagnostics(); });
	FEATURE_COMMAND("setPreparedScene",
		"Enable offline prepared-cell geometry and optional diagnostic local-light visibility tracing. Params: enabled (required boolean), preview (optional boolean), light (optional integer 0..255 baseline index). The selected placed-reference and base IDs must match exactly one evaluated live point light. PortalStrict uses current native receiver room membership; missing/stale metadata remains unavailable. Spot and particle lights remain excluded. GPU diagnostic colors distinguish missing tables, unresolved references, no match, ambiguity, unsupported light types, unknown rooms and room exclusions. Reads Data/SKSE/Plugins/CommunityShaders/RedstoneScene/cells/<plugin>/<local-id>/cell-layout.json on a worker. Unresolved materials are masked from tracing. Native shadows and GI remain authoritative; normal Save persists selections.",
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
	return (preparedTrace.IsRRDebugVisible() || captureDebug.load() || (settings.UsePreparedScene && settings.PreviewPreparedScene)) && !globals::game::isVR;
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
	preparedTrace.DrawRRDebugOverlay();
	if (settings.UsePreparedScene && settings.PreviewPreparedScene) {
		CS_GPU_PASS("RedstoneShadows::PreparedPreviewDraw");
		if (ImGui::Begin("Prepared cell shadow preview###RedstonePreparedPreview")) {
			static constexpr std::array views{ "Visibility", "Source base color", "World normal", "Source roughness", "Metalness", "Emission", "Material coverage", "Material AO", "PBR F0", "Opacity", "Direct diffuse", "Direct specular", "Direct combined and emission" };
			const auto selectedView = settings.UsePreparedMaterials ? std::min(settings.PreparedMaterialView, RedstoneShadow::DirectionalTrace::kCombinedDirectView) : 0u;
			const auto trace = preparedTrace.GetDiagnostics();
			ImGui::Text("%s | native shadows active", views[selectedView]);
			if (settings.UseAllPreparedLights && (selectedView >= RedstoneShadow::DirectionalTrace::kFirstDirectView || settings.UsePreparedDirectLighting))
				ImGui::Text("All prepared lights | submitted light count: %u | limit: %u", trace.value("requestedLightCount", 0u), RedstoneShadow::DirectionalTrace::kMaximumDirectLights);
			if (settings.UsePreparedDirectLighting) ImGui::Text("Direct scene mode | composite submissions: %llu | frame: %u", trace.value("sceneCompositeSubmissions", 0ull), trace.value("sceneCompositeFrame", 0u));
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
			if (settings.UsePreparedDirectLighting) ImGui::TextWrapped("The direct result is sent to the scene. Disable direct scene mode to inspect asynchronous previews.");
			else if (auto* image = preparedTrace.Preview()) {
				const auto width = std::max(ImGui::GetContentRegionAvail().x, 128.0f);
				const auto size = preparedTrace.PreviewSize();
				ImGui::Image(image, { width, width * float(size[1]) / float(std::max(size[0], 1u)) });
				preparedTrace.MarkPreviewDrawSubmitted();
			} else if (traceFailure.empty() && selectionFailure.empty()) ImGui::TextUnformatted("Waiting for prepared geometry, acceleration and a completed ray dispatch.");
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

void RedstoneShadows::DrawIndirectLightingSettings()
{
	const bool dxr = settings.UseDXRIndirect && !globals::game::isVR;
	if (ImGui::RadioButton("Raymarched indirect", !dxr)) {
		settings.UseDXRIndirect = false;
		globals::features::redstoneGI.SelectRenderer(true);
		preparedTrace.Reset();
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(globals::game::isVR);
	if (ImGui::RadioButton("DXR indirect", dxr)) {
		settings.UseDXRIndirect = true;
		settings.UsePreparedScene = settings.UsePreparedMaterials = true;
		resetRequested = true;
	}
	ImGui::EndDisabled();
	if (dxr) {
		ImGui::SeparatorText("DXR indirect lighting");
		int rays = int(settings.IndirectRays);
		if (ImGui::SliderInt("Indirect rays per pixel", &rays, 1, 8)) { settings.IndirectRays = uint32_t(rays); preparedTrace.Reset(); }
		ImGui::SliderFloat("Indirect intensity", &settings.IndirectStrength, 0.0f, 8.0f, "%.2fx");
		ImGui::TextWrapped("One-bounce diffuse and specular lighting from prepared geometry. More rays increase sampling work; intensity is independent. Raymarched indirect is inactive.");
		const auto trace = preparedTrace.GetDiagnostics();
		if (!trace.value("bakedRoomsKnown", false)) ImGui::TextWrapped("Prepared room data is not ready. Regenerate the current cell cache if it predates room metadata.");
		const auto failure = trace.value("failure", std::string{});
		if (!failure.empty()) ImGui::TextWrapped("DXR lighting unavailable: %s", failure.c_str());
	} else if (settings.UsePreparedDirectLighting) {
		ImGui::Checkbox("Connect raymarched indirect to prepared lighting", &settings.ConnectIndirectLighting);
		ImGui::SliderFloat("Prepared indirect intensity", &settings.IndirectStrength, 0.0f, 8.0f, "%.2fx");
	}
}

void RedstoneShadows::DrawRayRegenerationSettings()
{
	ImGui::Checkbox("Reconstruct prepared lighting", &settings.UseDirectRayRegeneration);
	int budget = int(settings.DirectLightingBudgetMB);
	if (ImGui::SliderInt("Prepared lighting buffer budget (MiB)", &budget, 64, 1024)) settings.DirectLightingBudgetMB = uint32_t(budget);
	ImGui::TextWrapped("This budget includes prepared lighting and reconstruction buffers. If it is insufficient, lighting uses raw signals. Provider-internal memory is additional.");
	const bool wasDebugging = preparedTrace.IsRRDebugVisible();
	preparedTrace.DrawRayRegenerationSettings(settings.UseDXRIndirect && !globals::game::isVR);
	if (!wasDebugging && preparedTrace.IsRRDebugVisible()) {
		globals::features::redstoneGI.SetRROutputOnly(false);
		globals::features::redstoneGI.SetRRInputsOnly(false);
	}
}

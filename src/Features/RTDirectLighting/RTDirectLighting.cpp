#include "RTDirectLighting.h"
#include "Globals.h"
#include "Features/DX12NativeScene/DX12NativeScene.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Features/RTIndirectLighting/RTIndirectLighting.h"
#include "Features/LightLimitFix.h"
#include "Deferred.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/DevBenchUx.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RTDirectLighting::Settings, RedstoneBlend, UsePreparedDirectLighting, UseAllPreparedLights, UseDirectRayRegeneration, DirectStrength, DiffuseStrength, SpecularStrength, EmissionStrength, SelectedDirectLight, NormalInfluence, DirectTraceResolution, DirectRRResolution, RRMaterialIsolation, RRGlossyRoughness)

std::pair<std::string, std::vector<std::string>> RTDirectLighting::GetFeatureSummary()
{
	return { "Direct diffuse/specular RT, raster composition, and RR reconstruction on completed native-scene resources.", {} };
}
void RTDirectLighting::ImportLegacySettings(json& root)
{
	if (root.contains(GetName())) return;
	auto it = root.find("RedstoneShadows");
	if (it == root.end() || !it->is_object()) return;
	json migrated = json::object();
	for (const char* field : { "RedstoneBlend", "UsePreparedDirectLighting", "UseAllPreparedLights", "UseDirectRayRegeneration",
			"DirectStrength", "DiffuseStrength", "SpecularStrength", "EmissionStrength", "SelectedDirectLight" })
		if (it->contains(field)) migrated[field] = (*it)[field];
	if (!migrated.contains("SelectedDirectLight") && it->contains("PreparedLight")) migrated["SelectedDirectLight"] = (*it)["PreparedLight"];
	root[GetName()] = std::move(migrated);
}
void RTDirectLighting::LoadSettings(json& value)
{
	Settings selected = value;
	selected.SelectedDirectLight = std::min(selected.SelectedDirectLight, 255u);
	selected.NormalInfluence = std::min(selected.NormalInfluence, 2u);
	selected.DirectTraceResolution = std::min(selected.DirectTraceResolution, 1u);
	selected.DirectRRResolution = std::min(selected.DirectRRResolution, 3u);
	selected.RRGlossyRoughness = std::isfinite(selected.RRGlossyRoughness) ? std::clamp(selected.RRGlossyRoughness, 0.04f, 1.0f) : 0.2f;
	selected.RedstoneBlend = std::isfinite(selected.RedstoneBlend) ? std::clamp(selected.RedstoneBlend, 0.0f, 1.0f) : 1.0f;
	for (auto* v : { &selected.DirectStrength, &selected.DiffuseStrength, &selected.SpecularStrength,
			&selected.EmissionStrength })
		*v = std::isfinite(*v) ? std::clamp(*v, 0.0f, 8.0f) : 1.0f;
	settings = selected;
	lastFrame.reset();
}
void RTDirectLighting::SaveSettings(json& value) { value = settings; }
void RTDirectLighting::RestoreDefaultSettings() { settings = {}; lastFrame.reset(); }
void RTDirectLighting::DrawSettings() { DrawDirectLightingSettings(); }
void RTDirectLighting::ClearRRDebugOverlay() { globals::features::dx12NativeScene.Tracer().ClearRRDebugOverlay(); }
void RTDirectLighting::DrawOverlay() { if (IsOverlayVisible()) globals::features::dx12NativeScene.Tracer().DrawRRDebugOverlay(); }
bool RTDirectLighting::IsOverlayVisible() const { return globals::features::dx12NativeScene.loaded && globals::features::dx12NativeScene.Tracer().IsRRDebugVisible() && !globals::features::rtIndirectLighting.DebugAtlasActive() && !globals::game::isVR; }
void RTDirectLighting::OnSceneTransitionReset(bool) { lastFrame.reset(); }
void RTDirectLighting::OnRuntimeDisabled()
{
	lastFrame.reset();
	globals::features::dx12NativeScene.Tracer().ClearRRDebugOverlay();
}
void RTDirectLighting::ClearShaderCache()
{
	lastFrame.reset();
	globals::features::dx12NativeScene.Tracer().ReloadShaders();
}
json RTDirectLighting::GetDiagnostics()
{
	return { { "directLightingEnabled", settings.UsePreparedDirectLighting }, { "selectedDirectLight", settings.SelectedDirectLight },
		{ "rasterRTBlend", settings.RedstoneBlend }, { "trace", globals::features::dx12NativeScene.Tracer().GetDiagnostics() } };
}

void RTDirectLighting::DrawDirectLightingSettings()
{
    if (ImGui::Checkbox("RT direct lighting", &settings.UsePreparedDirectLighting)) {
        if (!settings.UsePreparedDirectLighting)
            globals::features::rtIndirectLighting.SelectHybrid();
        globals::features::dx12NativeScene.Tracer().Reset();
    }

    ImGui::SeparatorText("Direct lighting");
    int normalMode = static_cast<int>(settings.NormalInfluence);
    if (ImGui::Combo("Normal influence", &normalMode, "Legacy flat (compatibility)\0Geometry normals\0Full material normals (True PBR)\0")) {
        settings.NormalInfluence = static_cast<uint32_t>(normalMode);
        globals::features::dx12NativeScene.Tracer().Reset();
    }
    ImGui::TextWrapped("Geometry mode ignores normal maps. Full material mode uses loaded shading normals. Legacy flat retains the old response with zero direct specular.");
    int traceRes = static_cast<int>(settings.DirectTraceResolution);
    if (ImGui::Combo("Direct trace resolution", &traceRes, "Full\0Half (depth-aware resolve)\0")) {
        settings.DirectTraceResolution = uint32_t(traceRes);
        globals::features::dx12NativeScene.Tracer().Reset();
    }
    int rrRes = static_cast<int>(settings.DirectRRResolution);
    if (ImGui::Combo("Direct RR resolution", &rrRes, "Full\0Half\0Quarter\0Use shared RR setting (legacy)\0")) {
        settings.DirectRRResolution = uint32_t(rrRes);
        globals::features::dx12NativeScene.Tracer().Reset();
    }
    ImGui::SliderFloat("Raster / RT blend", &settings.RedstoneBlend, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Direct lighting strength", &settings.DirectStrength, 0.0f, 8.0f, "%.2fx");
    if (ImGui::Checkbox("All prepared cell lights (direct only)", &settings.UseAllPreparedLights)) globals::features::dx12NativeScene.Tracer().Reset();

    const auto prepared = globals::features::dx12NativeScene.GetPreparedDiagnostics();
    int light = int(settings.SelectedDirectLight);
    const auto maximumLight = std::max(0, int(prepared.value("lightCount", 0ull)) - 1);
    if (ImGui::SliderInt("Prepared baseline light", &light, 0, maximumLight)) {
        settings.SelectedDirectLight = uint32_t(light);
        globals::features::dx12NativeScene.Tracer().Reset();
    }

    if (ImGui::TreeNodeEx("Diffuse", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Direct diffuse strength", &settings.DiffuseStrength, 0.0f, 8.0f, "%.2fx");
        ImGui::TreePop();
    }
    if (ImGui::TreeNodeEx("Specular", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Direct specular strength", &settings.SpecularStrength, 0.0f, 8.0f, "%.2fx");
        ImGui::TreePop();
    }
    ImGui::SliderFloat("Prepared emission strength", &settings.EmissionStrength, 0.0f, 8.0f, "%.2fx");
    if (ImGui::Button("Reset lighting strengths"))
        settings.DirectStrength = settings.DiffuseStrength = settings.SpecularStrength = settings.EmissionStrength = 1.0f;
    ImGui::TextWrapped("Direct strength scales diffuse and specular; emission is independent. Strength changes apply to the next trace/preview dispatch. Normal Save persists values.");

    if (settings.UsePreparedDirectLighting) {
        ImGui::TextWrapped("Requires prepared geometry and material maps. Supported depth-matched opaque pixels receive direct diffuse/specular and emission; others retain raster output. Missing or unsupported lights keep native output. Unlisted direct lights remain absent at full RT blend; connected indirect lighting is selected separately. Native shadow production remains active.");
        const auto trace = globals::features::dx12NativeScene.Tracer().GetDiagnostics();
        ImGui::Text("Submitted light count: %u | limit: %u", trace.value("requestedLightCount", 0u), DX12Native::DirectionalTrace::kMaximumDirectLights);
        ImGui::Text("Scene submissions: %llu | last frame: %u | budget deferrals: %llu | busy frames: %llu",
            trace.value("sceneCompositeSubmissions", 0ull), trace.value("sceneCompositeFrame", 0u), trace.value("budgetDeferrals", 0ull), trace.value("busyFrames", 0ull));
        const auto failure = trace.value("failure", std::string{});
        if (!failure.empty()) ImGui::TextWrapped("Direct lighting unavailable: %s", failure.c_str());
    }
    const auto selectionFailure = globals::features::dx12NativeScene.Tracer().GetDiagnostics().value("lightSelectionFailure", std::string{});
    if (!selectionFailure.empty()) ImGui::TextWrapped("Light set unavailable: %s", selectionFailure.c_str());
    ImGui::TextWrapped("Static prepared-light baseline is independent of LLF radiance. Animated lights, PortalStrict and reference overrides are not yet supported and preserve raster fallback. The 32-light trace limit remains.");
}


void RTDirectLighting::DrawRayRegenerationSettings()
{
	ImGui::Checkbox("Reconstruct prepared lighting", &settings.UseDirectRayRegeneration);
	if (ImGui::Checkbox("Separate RR history by material type", &settings.RRMaterialIsolation))
		globals::features::dx12NativeScene.Tracer().Reset();
	if (settings.RRMaterialIsolation && ImGui::SliderFloat("Glossy material roughness threshold", &settings.RRGlossyRoughness, 0.04f, 1.0f, "%.2f"))
		globals::features::dx12NativeScene.Tracer().Reset();
	ImGui::TextWrapped("RR classes: 0 ordinary opaque, 1 glossy, 2 metallic, 3 reserved.");
	const bool wasDebugging = globals::features::dx12NativeScene.Tracer().IsRRDebugVisible();
	globals::features::dx12NativeScene.Tracer().DrawRayRegenerationSettings(globals::features::rtIndirectLighting.UsesRTExclusive());
	if (!wasDebugging && globals::features::dx12NativeScene.Tracer().IsRRDebugVisible()) {
		globals::features::redstoneGI.SetRROutputOnly(false);
		globals::features::redstoneGI.SetRRInputsOnly(false);
	}
}

void RTDirectLighting::OnAfterDeferredLighting(ID3D11UnorderedAccessView* target, const std::array<uint32_t, 2>& size)
{
	CS_GPU_PASS("RTDirectLighting::PreparedLighting");
	if (!loaded || !WantsComposite() || !target || !globals::features::dx12NativeScene.loaded ||
		!globals::features::dx12NativeScene.settings.UsePreparedMaterials || globals::game::isVR || globals::state->isMapMenuOpen) return;
	if (target != Util::AsReal(globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].UAV) ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))) return;
	auto& native = globals::features::dx12NativeScene;
	auto& gi = globals::features::rtIndirectLighting;
	const bool rtExclusive = gi.UsesRTExclusive();
	auto snapshot = native.GetPublishedScene();
	if (!snapshot) return;
	const auto frame = globals::game::graphicsState->GetFrameCount();
	if (lastFrame && *lastFrame == frame) return;
	if (!settings.UseAllPreparedLights && settings.SelectedDirectLight >= snapshot->cell->lights.size()) return;
	auto backend = Feature::FindGraphicsBackend();
	auto& tracer = native.Tracer();
	if (!backend || !tracer.Initialize(*backend)) return;
	// LLF is retained only for the existing opaque-room generation/stamp, not for lighting.
	const auto lights = globals::features::lightLimitFix.GetLightView(frame);
	const float3 light = settings.SelectedDirectLight < snapshot->cell->lights.size() ? snapshot->cell->lights[settings.SelectedDirectLight] : float3{};
	const auto identity = settings.SelectedDirectLight < snapshot->lightReferences.size() ? snapshot->lightReferences[settings.SelectedDirectLight] : std::array<uint32_t, 2>{};
	auto* metadata = Util::AsReal(globals::game::renderer->GetRuntimeData().renderTargets[MASKS2].SRV);
	D3D11_TEXTURE2D_DESC desc{};
	if (!Util::GetTexture2DDesc(metadata, desc) || desc.Format != DXGI_FORMAT_R16G16B16A16_UNORM) metadata = nullptr;
	tracer.Dispatch(snapshot->acceleration, size, light, frame, native.GetLightingBudgetBytes(), true, lights,
		DX12Native::DirectionalTrace::kCombinedDirectView, target, identity[0], identity[1], metadata,
		settings.UseAllPreparedLights ? std::span<const std::array<uint32_t, 2>>(snapshot->lightReferences) : std::span<const std::array<uint32_t, 2>>{},
		settings.UseAllPreparedLights, { settings.DirectStrength, settings.DiffuseStrength, settings.SpecularStrength, settings.EmissionStrength },
		gi.loaded && gi.settings.ConnectHybrid && !rtExclusive, gi.settings.IndirectStrength, settings.UseDirectRayRegeneration,
		rtExclusive ? gi.settings.IndirectRays : 0u, snapshot->roomReferences, settings.RedstoneBlend, settings.NormalInfluence,
		settings.UseAllPreparedLights ? std::span<const DX12Native::PreparedPointLight>(snapshot->cell->nativeLights) :
			std::span<const DX12Native::PreparedPointLight>(snapshot->cell->nativeLights).subspan(settings.SelectedDirectLight, 1),
		settings.DirectTraceResolution, gi.settings.RTTraceResolution, settings.DirectRRResolution,
		settings.RRMaterialIsolation, settings.RRGlossyRoughness);
	lastFrame = frame;
}

void RTDirectLighting::RegisterUxActions()
{
	FEATURE_COMMAND("setPreparedDirectLighting", "Configure direct lighting, light selection, reconstruction and budget.",
		[](Feature* self, const json& args) {
			auto& d = *static_cast<RTDirectLighting*>(self);
			auto& n = globals::features::dx12NativeScene;
			d.settings.UsePreparedDirectLighting = args.at("enabled").get<bool>();
			if (!d.settings.UsePreparedDirectLighting) globals::features::rtIndirectLighting.SelectHybrid();
			if (d.settings.UsePreparedDirectLighting) n.RequestPreparedScene();
			if (args.contains("allLights")) d.settings.UseAllPreparedLights = args.at("allLights").get<bool>();
			if (args.contains("normalMode")) d.settings.NormalInfluence = std::min(args.at("normalMode").get<uint32_t>(), 2u);
			if (args.contains("traceResolution")) d.settings.DirectTraceResolution = std::min(args.at("traceResolution").get<uint32_t>(), 1u);
			if (args.contains("rrResolution")) d.settings.DirectRRResolution = std::min(args.at("rrResolution").get<uint32_t>(), 3u);
			if (args.contains("rrMaterialIsolation")) d.settings.RRMaterialIsolation = args.at("rrMaterialIsolation").get<bool>();
			if (args.contains("rrGlossyRoughness")) d.settings.RRGlossyRoughness = std::clamp(args.at("rrGlossyRoughness").get<float>(), 0.04f, 1.0f);
			if (args.contains("indirect")) globals::features::rtIndirectLighting.settings.ConnectHybrid = args.at("indirect").get<bool>();
			if (args.contains("rr")) d.settings.UseDirectRayRegeneration = args.at("rr").get<bool>();
			if (args.contains("bufferBudgetMB")) n.settings.DirectLightingBudgetMB = std::clamp(args.at("bufferBudgetMB").get<uint32_t>(), 64u, 1024u);
			n.Tracer().Reset();
		});
	FEATURE_COMMAND("setLightingStrengths", "Set existing direct, diffuse, specular, emission and indirect intensity.",
		[](Feature* self, const json& args) {
			auto& s = static_cast<RTDirectLighting*>(self)->settings;
			for (const char* key : { "direct", "diffuse", "specular", "emission", "indirect" })
				if (args.contains(key) && !std::isfinite(args.at(key).get<float>())) throw std::invalid_argument("Strength must be finite");
			auto apply = [&](const char* key, float& v) { if (args.contains(key)) v = std::clamp(args.at(key).get<float>(), 0.0f, 8.0f); };
			apply("direct", s.DirectStrength);
			apply("diffuse", s.DiffuseStrength);
			apply("specular", s.SpecularStrength);
			apply("emission", s.EmissionStrength);
			apply("indirect", globals::features::rtIndirectLighting.settings.IndirectStrength);
		});
	FEATURE_COMMAND("setPreparedRRDebug", "Choose prepared ray-regeneration debug view.",
		[](Feature*, const json& args) {
			const auto source = args.at("source").get<std::string>();
			if (source != "off" && source != "inputs" && source != "outputs") throw std::invalid_argument("Invalid RR debug source");
			globals::features::dx12NativeScene.Tracer().SetRRDebug(source == "inputs" ? 1u : source == "outputs" ? 2u : 0u,
				args.value("signal", 0u), args.value("buffer", 2u), args.value("exposure", 0.0f));
			if (source != "off") {
				globals::features::redstoneGI.SetRROutputOnly(false);
				globals::features::redstoneGI.SetRRInputsOnly(false);
			}
		});
}

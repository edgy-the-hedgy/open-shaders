#include "RTDirectLighting.h"
#include "Globals.h"
#include "Features/DX12NativeScene/DX12NativeScene.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Features/LightLimitFix.h"
#include "Deferred.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/DevBenchUx.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RTDirectLighting::Settings, RedstoneBlend, UsePreparedDirectLighting, UseAllPreparedLights, ConnectIndirectLighting, UseDirectRayRegeneration, UseDXRIndirect, IndirectRays, DirectStrength, DiffuseStrength, SpecularStrength, EmissionStrength, IndirectStrength, SelectedDirectLight)

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
	for (const char* field : { "RedstoneBlend", "UsePreparedDirectLighting", "UseAllPreparedLights", "ConnectIndirectLighting",
			"UseDirectRayRegeneration", "UseDXRIndirect", "IndirectRays", "DirectStrength", "DiffuseStrength",
			"SpecularStrength", "EmissionStrength", "IndirectStrength", "SelectedDirectLight" })
		if (it->contains(field)) migrated[field] = (*it)[field];
	if (!migrated.contains("SelectedDirectLight") && it->contains("PreparedLight")) migrated["SelectedDirectLight"] = (*it)["PreparedLight"];
	root[GetName()] = std::move(migrated);
}
void RTDirectLighting::LoadSettings(json& value)
{
	Settings selected = value;
	selected.IndirectRays = std::clamp(selected.IndirectRays, 1u, 8u);
	selected.SelectedDirectLight = std::min(selected.SelectedDirectLight, 255u);
	selected.RedstoneBlend = std::isfinite(selected.RedstoneBlend) ? std::clamp(selected.RedstoneBlend, 0.0f, 1.0f) : 1.0f;
	for (auto* v : { &selected.DirectStrength, &selected.DiffuseStrength, &selected.SpecularStrength,
			&selected.EmissionStrength, &selected.IndirectStrength })
		*v = std::isfinite(*v) ? std::clamp(*v, 0.0f, 8.0f) : 1.0f;
	settings = selected;
	lastFrame.reset();
}
void RTDirectLighting::SaveSettings(json& value) { value = settings; }
void RTDirectLighting::RestoreDefaultSettings() { settings = {}; lastFrame.reset(); }
void RTDirectLighting::DrawSettings() { DrawDirectLightingSettings(); }
int RTDirectLighting::GetIndirectLightingPriority() const { return loaded && settings.UseDXRIndirect && !globals::game::isVR ? 200 : -1; }
void RTDirectLighting::ClearRRDebugOverlay() { globals::features::dx12NativeScene.Tracer().ClearRRDebugOverlay(); }
void RTDirectLighting::DrawOverlay() { if (IsOverlayVisible()) globals::features::dx12NativeScene.Tracer().DrawRRDebugOverlay(); }
bool RTDirectLighting::IsOverlayVisible() const { return globals::features::dx12NativeScene.loaded && globals::features::dx12NativeScene.Tracer().IsRRDebugVisible() && !globals::game::isVR; }
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
		{ "useDXRIndirect", settings.UseDXRIndirect }, { "indirectRays", settings.IndirectRays },
		{ "rasterRTBlend", settings.RedstoneBlend }, { "trace", globals::features::dx12NativeScene.Tracer().GetDiagnostics() } };
}

void RTDirectLighting::DrawDirectLightingSettings()
{
    if (ImGui::Checkbox("RT direct lighting", &settings.UsePreparedDirectLighting)) globals::features::dx12NativeScene.Tracer().Reset();

    ImGui::SeparatorText("Direct lighting");
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
        settings.DirectStrength = settings.DiffuseStrength = settings.SpecularStrength = settings.EmissionStrength = settings.IndirectStrength = 1.0f;
    ImGui::TextWrapped("Direct strength scales diffuse and specular; emission is independent. Strength changes apply to the next trace/preview dispatch. Normal Save persists values.");

    if (settings.UsePreparedDirectLighting || settings.UseDXRIndirect) {
        ImGui::TextWrapped("Requires prepared geometry and material maps. Supported depth-matched opaque pixels receive direct diffuse/specular and emission; others retain raster output. Missing or unsupported lights keep native output. Unlisted direct lights remain absent at full RT blend; connected indirect lighting is selected separately. Native shadow production remains active.");
        const auto trace = globals::features::dx12NativeScene.Tracer().GetDiagnostics();
        if (settings.UseDXRIndirect && trace.value("cachedSceneBindings", false) && !trace.value("bakedRoomsKnown", false))
            ImGui::TextWrapped("DXR indirect requires regenerated room metadata. Rebuild the current cell's geometry cache.");
        ImGui::Text("Submitted light count: %u | limit: %u", trace.value("requestedLightCount", 0u), DX12Native::DirectionalTrace::kMaximumDirectLights);
        ImGui::Text("Scene submissions: %llu | last frame: %u | budget deferrals: %llu | busy frames: %llu",
            trace.value("sceneCompositeSubmissions", 0ull), trace.value("sceneCompositeFrame", 0u), trace.value("budgetDeferrals", 0ull), trace.value("busyFrames", 0ull));
        const auto failure = trace.value("failure", std::string{});
        if (!failure.empty()) ImGui::TextWrapped("Direct lighting unavailable: %s", failure.c_str());
    }
    const auto selectionFailure = globals::features::dx12NativeScene.Tracer().GetDiagnostics().value("lightSelectionFailure", std::string{});
    if (!selectionFailure.empty()) ImGui::TextWrapped("Light set unavailable: %s", selectionFailure.c_str());
    ImGui::TextWrapped("Visibility and direct-light previews require a live matching placed-reference/base light. PortalStrict uses the native receiver-room mask; unsupported materials and saved/moved objects are not validated.");
}


void RTDirectLighting::DrawIndirectLightingSettings()
{
	const bool dxr = settings.UseDXRIndirect && !globals::game::isVR;
	if (ImGui::RadioButton("Raymarched indirect", !dxr)) {
		settings.UseDXRIndirect = false;
		globals::features::redstoneGI.SelectRenderer(true);
		globals::features::dx12NativeScene.Tracer().Reset();
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(globals::game::isVR);
	if (ImGui::RadioButton("DXR indirect", dxr)) {
		settings.UseDXRIndirect = true;
		globals::features::dx12NativeScene.RequestPreparedScene();
	}
	ImGui::EndDisabled();
	if (dxr) {
		ImGui::SeparatorText("DXR indirect lighting");
		int rays = int(settings.IndirectRays);
		if (ImGui::SliderInt("Indirect rays per pixel", &rays, 1, 8)) { settings.IndirectRays = uint32_t(rays); globals::features::dx12NativeScene.Tracer().Reset(); }
		ImGui::SliderFloat("Indirect intensity", &settings.IndirectStrength, 0.0f, 8.0f, "%.2fx");
		ImGui::TextWrapped("One-bounce diffuse and specular lighting from prepared geometry. More rays increase sampling work; intensity is independent. Raymarched indirect is inactive.");
		const auto trace = globals::features::dx12NativeScene.Tracer().GetDiagnostics();
		if (!trace.value("bakedRoomsKnown", false)) ImGui::TextWrapped("Prepared room data is not ready. Regenerate the current cell cache if it predates room metadata.");
		const auto failure = trace.value("failure", std::string{});
		if (!failure.empty()) ImGui::TextWrapped("DXR lighting unavailable: %s", failure.c_str());
	} else if (settings.UsePreparedDirectLighting) {
		ImGui::Checkbox("Connect raymarched indirect to prepared lighting", &settings.ConnectIndirectLighting);
		ImGui::SliderFloat("Prepared indirect intensity", &settings.IndirectStrength, 0.0f, 8.0f, "%.2fx");
	}
}

void RTDirectLighting::DrawRayRegenerationSettings()
{
	ImGui::Checkbox("Reconstruct prepared lighting", &settings.UseDirectRayRegeneration);
	const bool wasDebugging = globals::features::dx12NativeScene.Tracer().IsRRDebugVisible();
	globals::features::dx12NativeScene.Tracer().DrawRayRegenerationSettings(settings.UseDXRIndirect && !globals::game::isVR);
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
	auto snapshot = native.GetPublishedScene();
	if (!snapshot) return;
	const auto frame = globals::game::graphicsState->GetFrameCount();
	if (lastFrame && *lastFrame == frame) return;
	if (!settings.UseAllPreparedLights && settings.SelectedDirectLight >= snapshot->cell->lights.size()) return;
	auto backend = Feature::FindGraphicsBackend();
	auto& tracer = native.Tracer();
	if (!backend || !tracer.Initialize(*backend)) return;
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
		settings.ConnectIndirectLighting && !settings.UseDXRIndirect, settings.IndirectStrength, settings.UseDirectRayRegeneration,
		settings.UseDXRIndirect ? settings.IndirectRays : 0u, snapshot->roomReferences, settings.RedstoneBlend);
	lastFrame = frame;
}

void RTDirectLighting::RegisterUxActions()
{
	FEATURE_COMMAND("setPreparedDirectLighting", "Configure direct lighting, light selection, reconstruction and budget.",
		[](Feature* self, const json& args) {
			auto& d = *static_cast<RTDirectLighting*>(self);
			auto& n = globals::features::dx12NativeScene;
			d.settings.UsePreparedDirectLighting = args.at("enabled").get<bool>();
			if (d.settings.UsePreparedDirectLighting) n.RequestPreparedScene();
			if (args.contains("allLights")) d.settings.UseAllPreparedLights = args.at("allLights").get<bool>();
			if (args.contains("indirect")) d.settings.ConnectIndirectLighting = args.at("indirect").get<bool>();
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
			apply("indirect", s.IndirectStrength);
		});
	FEATURE_COMMAND("setIndirectProducer", "Select existing raymarched/DXR indirect producer and ray count.",
		[](Feature* self, const json& args) {
			auto& d = *static_cast<RTDirectLighting*>(self);
			const auto producer = args.at("producer").get<std::string>();
			if (producer != "dxr" && producer != "raymarched") throw std::invalid_argument("Producer must be dxr or raymarched");
			d.settings.UseDXRIndirect = producer == "dxr";
			d.settings.IndirectRays = std::clamp(args.value("rays", d.settings.IndirectRays), 1u, 8u);
			if (d.settings.UseDXRIndirect) globals::features::dx12NativeScene.RequestPreparedScene();
			else globals::features::redstoneGI.SelectRenderer(true);
			globals::features::dx12NativeScene.Tracer().Reset();
		});
	FEATURE_COMMAND("setLightingSampling", "Adjust RedstoneGI raymarched sampling slices.",
		[](Feature*, const json& args) {
			auto& gi = globals::features::redstoneGI;
			const auto current = gi.GetLightingSamples();
			gi.SetLightingSamples(args.value("diffuseSlices", current[0]), args.value("specularSlices", current[1]));
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

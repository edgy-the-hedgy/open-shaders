#include "RTIndirectLighting.h"
#include "Globals.h"
#include "Features/RTDirectLighting/RTDirectLighting.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Features/RTComposer/RTComposer.h"
#include "Features/DX12NativeScene/DX12NativeScene.h"
#include "Utils/DevBenchUx.h"
#include <cmath>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RTIndirectLighting::Settings, UseRTExclusive, IndirectRays, IndirectStrength, ConnectHybrid)

std::pair<std::string, std::vector<std::string>> RTIndirectLighting::GetFeatureSummary()
{
	return { "Choose RT-exclusive GI (requires RT direct) or Hybrid GI (raymarched), with existing diffuse/specular occlusion and ray regeneration.", {} };
}

void RTIndirectLighting::ImportLegacySettings(json& root)
{
	if (root.contains(GetName())) return;
	const json* source = nullptr;
	for (const char* name : { "RTDirectLighting", "RedstoneShadows" }) {
		const auto found = root.find(name);
		if (found != root.end() && found->is_object() &&
			(found->contains("UseDXRIndirect") || found->contains("IndirectRays") ||
				found->contains("IndirectStrength") || found->contains("ConnectIndirectLighting"))) {
			source = &*found;
			break;
		}
	}
	if (!source) return;
	json migrated = json::object();
	migrated["UseRTExclusive"] = source->value("UseDXRIndirect", false);
	migrated["IndirectRays"] = source->value("IndirectRays", 1u);
	migrated["IndirectStrength"] = source->value("IndirectStrength", 1.0f);
	migrated["ConnectHybrid"] = source->value("ConnectIndirectLighting", true);
	root[GetName()] = std::move(migrated);
}

void RTIndirectLighting::LoadSettings(json& value)
{
	Settings selected = value;
	selected.IndirectRays = std::clamp(selected.IndirectRays, 1u, 8u);
	selected.RTTraceResolution = std::min(selected.RTTraceResolution, 4u);
	selected.HybridResolution = std::min(selected.HybridResolution, 3u);
	selected.IndirectStrength = std::isfinite(selected.IndirectStrength) ? std::clamp(selected.IndirectStrength, 0.0f, 8.0f) : 1.0f;
	settings = selected;
	if (settings.UseRTExclusive) {
		// Keep an old RT-exclusive selection valid after migration.
		globals::features::rtDirectLighting.settings.UsePreparedDirectLighting = true;
		globals::features::dx12NativeScene.RequestPreparedScene();
	}
	// Hybrid retains the previously saved screen-space GI selection on load.
	// Explicitly choosing Hybrid in the UI or DevBench selects the RGI backend.
}

void RTIndirectLighting::SaveSettings(json& value) { value = settings; }
void RTIndirectLighting::RestoreDefaultSettings()
{
	settings = {};
}

bool RTIndirectLighting::UsesRTExclusive() const
{
	const auto& direct = globals::features::rtDirectLighting;
	return loaded && !globals::features::rtComposer.BlocksRTIndirect() && settings.UseRTExclusive && !globals::game::isVR &&
		direct.loaded && direct.settings.UsePreparedDirectLighting;
}

int RTIndirectLighting::GetIndirectLightingPriority() const
{
	return UsesRTExclusive() ? 200 : -1;
}

void RTIndirectLighting::SelectHybrid()
{
	if (settings.UseRTExclusive) globals::features::dx12NativeScene.Tracer().Reset();
	settings.UseRTExclusive = false;
	globals::features::redstoneGI.SelectRenderer(false); // legacy DX11 HybridGI remains disabled
}

void RTIndirectLighting::SelectRTExclusive()
{
	if (globals::game::isVR || !globals::features::rtDirectLighting.loaded) return;
	settings.UseRTExclusive = true;
	globals::features::rtDirectLighting.settings.UsePreparedDirectLighting = true;
	globals::features::dx12NativeScene.RequestPreparedScene();
	globals::features::dx12NativeScene.Tracer().Reset();
}

void RTIndirectLighting::DrawSettings()
{
    const auto& composer = globals::features::rtComposer;
    if (composer.TierControlsLocked()) {
        ImGui::SeparatorText("Indirect lighting - fixed composer tier");
        ImGui::Text("Selected: %s", RTComposer::ModeName(composer.composition.mode));
        ImGui::TextWrapped("%s", RTComposer::IndirectTierDescription(composer.composition.mode));
        ImGui::TextWrapped("Indirect controls are locked. Choose Custom in RT Composer to edit GI and RR settings.");
        if (composer.WantsNativeHybrid() && !composer.DX12HybridReady())
            ImGui::TextWrapped("Native DX12 HybridGI is not ready; Shadow Only fallback is active.");
        const auto trace = globals::features::dx12NativeScene.Tracer().GetDiagnostics();
        ImGui::Text("Native GI/AO dispatches: %llu / %llu",
            static_cast<unsigned long long>(trace.value("nativeHybridGIDispatches", uint64_t{0})),
            static_cast<unsigned long long>(trace.value("nativeHybridAODispatches", uint64_t{0})));
        return;
    }
    if (composer.composition.enabled &&
        composer.composition.mode == RTComposer::CompositorMode::Custom &&
        composer.composition.customHybridShadowed) {
        ImGui::SeparatorText("Custom native DX12 HybridGI");
        ImGui::TextWrapped("Native Hybrid-Shadowed is selected in RT Composer. Legacy DX11 HybridGI and the separate RT-exclusive provider are unavailable in this backend.");
        ImGui::TextWrapped("Adjust native GI/AO sampling, RR and post-filtering in RT Composer. Material and lighting estimator settings remain editable below.");
        globals::features::redstoneGI.DrawSettings();
        return;
    }
    const bool blocked = composer.BlocksRTIndirect();
    if (blocked) ImGui::TextWrapped("Unavailable in RT Composer mode: RT-exclusive GI and DX11 HybridGI are disabled. Saved selections are retained; baseline SSGI is the permitted provider.");
    ImGui::BeginDisabled(blocked);
	ImGui::TextUnformatted("How would you like your GI to be rendered?");
	ImGui::BeginDisabled(globals::game::isVR || !globals::features::rtDirectLighting.loaded);
	if (ImGui::RadioButton("RT-exclusive", settings.UseRTExclusive)) SelectRTExclusive();
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::RadioButton("Hybrid", !settings.UseRTExclusive)) SelectHybrid();

	if (globals::game::isVR) ImGui::TextWrapped("RT-exclusive is unavailable in VR.");
	else if (!globals::features::rtDirectLighting.loaded)
		ImGui::TextWrapped("RT-exclusive requires the RT Direct Lighting feature to be enabled.");

	if (settings.UseRTExclusive) {
		ImGui::TextWrapped("RT-exclusive uses the prepared native scene for indirect diffuse and specular. RT Direct Lighting is required. Direct and indirect run separate GPU compute dispatches using shared retained scene and receiver resources.");
		int rays = int(settings.IndirectRays);
		if (ImGui::SliderInt("Indirect rays per pixel", &rays, 1, 8)) {
			settings.IndirectRays = uint32_t(rays);
			globals::features::dx12NativeScene.Tracer().Reset();
		}
		ImGui::TextWrapped("RT-exclusive indirect traces at native resolution or native checkerboard, matching direct lighting. Its independent resolution selector is retired.");
		ImGui::SliderFloat("Indirect intensity", &settings.IndirectStrength, 0.0f, 8.0f, "%.2fx");
		const auto trace = globals::features::dx12NativeScene.Tracer().GetDiagnostics();
		if (!trace.value("bakedRoomsKnown", false))
			ImGui::TextWrapped("Prepared room data is not ready. Regenerate the cell cache if it predates room metadata.");
		const auto failure = trace.value("failure", std::string{});
		if (!failure.empty()) ImGui::TextWrapped("RT-exclusive lighting unavailable: %s", failure.c_str());
	} else {
		ImGui::TextWrapped("Hybrid GI/AO uses the same producer-derived resolution and checkerboard policy selected in RT Composer.");
		ImGui::TextWrapped("Hybrid uses raymarched indirect diffuse/specular and occlusion with the raster baseline, optionally combined with RT Direct Lighting.");
		if (globals::features::rtDirectLighting.loaded && globals::features::rtDirectLighting.settings.UsePreparedDirectLighting) {
			ImGui::Checkbox("Connect Hybrid GI to prepared direct lighting", &settings.ConnectHybrid);
			ImGui::SliderFloat("Prepared indirect intensity", &settings.IndirectStrength, 0.0f, 8.0f, "%.2fx");
		}
		globals::features::redstoneGI.DrawSettings();
	}
    ImGui::EndDisabled();
}


void RTIndirectLighting::DrawUnifiedDebugSettings()
{
	ImGui::SeparatorText("RT Lighting Diagnostics");
	int view = int(debugView);
	static constexpr const char* views =
		"Off\0Direct diffuse (RR input)\0Direct diffuse (RR output)\0"
		"Direct specular (RR input)\0Direct specular (RR output)\0"
		"Direct combined\0RT indirect diffuse\0RT indirect specular\0RT indirect combined\0"
		"Hybrid GI combined\0Hybrid diffuse AO\0Hybrid specular occlusion\0Surface validity\0"
		"All signals (native + Hybrid)\0Raw direct lighting (RR independent)\0";
	bool changed = ImGui::Combo("Debug view", &view, views);
	changed |= ImGui::SliderFloat("Debug exposure (EV)", &debugExposure, -8.0f, 8.0f, "%.1f");
	if (changed) {
		debugView = uint32_t(std::clamp(view, 0, 14));
		auto& tracer = globals::features::dx12NativeScene.Tracer();
		auto& hybrid = globals::features::redstoneGI;
		tracer.ClearRRDebugOverlay();
		hybrid.SetRROutputOnly(false);
		hybrid.SetRRInputsOnly(false);
		if (debugView >= 1 && debugView <= 8) {
			static constexpr std::array sources{ 1u, 2u, 1u, 2u, 2u, 2u, 2u, 2u };
			static constexpr std::array signals{ 0u, 0u, 1u, 1u, 2u, 3u, 4u, 5u };
			tracer.SetRRDebug(sources[debugView - 1], signals[debugView - 1], 2u, debugExposure);
		} else if (debugView == 12) {
			tracer.SetRRDebug(2, 6, 15, debugExposure);
		} else if (debugView >= 9 && debugView <= 11) {
			hybrid.SetRRDebugView(debugView == 9 ? RedstoneGI::RRDebugView::Combined :
				debugView == 10 ? RedstoneGI::RRDebugView::DiffuseOcclusion : RedstoneGI::RRDebugView::SpecularOcclusion);
			hybrid.SetRROutputOnly(true);
		} else if (debugView == 14) {
			tracer.SetRRDebug(3, 0, 16, debugExposure);
		} else if (debugView == 13) {
			// RGI's already implemented 2x2 atlas and the native combined signal
			// are displayed together; stale providers remain explicitly unavailable.
			hybrid.SetRRDebugView(RedstoneGI::RRDebugView::AllSignals);
			hybrid.SetRROutputOnly(true);
			tracer.SetRRDebug(2, 2, 13, debugExposure);
		}
	}
	if (debugView == 13) {
		if (ImGui::BeginTable("RTLightingSignalAtlas", 2, ImGuiTableFlags_SizingStretchSame)) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted("Native direct diffuse + specular");
			if (auto* native = globals::features::dx12NativeScene.Tracer().RRDebugPreview())
				ImGui::Image(reinterpret_cast<ImTextureID>(native), ImVec2(320, 180));
			else ImGui::TextUnformatted("No current native RR signals");
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted("Hybrid GI / occlusion 2x2");
			if (auto* hybrid = globals::features::redstoneGI.RRDebugPreview())
				ImGui::Image(reinterpret_cast<ImTextureID>(hybrid), ImVec2(320, 180));
			else ImGui::TextUnformatted("Hybrid signal atlas not active");
			ImGui::EndTable();
		}
	}
	ImGui::TextWrapped("View selection affects only diagnostics. Existing shader resources and RR histories remain owned by their producers.");
}

json RTIndirectLighting::GetDiagnostics()
{
	const auto& direct = globals::features::rtDirectLighting;
	return { { "mode", settings.UseRTExclusive ? "rt-exclusive" : "hybrid" }, { "unifiedDebugView", debugView },
		{ "rtExclusiveActive", UsesRTExclusive() },
        { "indirectDisabledByComposer", globals::features::rtComposer.BlocksRTIndirect() },
        { "indirectAvailability", globals::features::rtComposer.BlocksRTIndirect() ? "disabled-by-compositor-policy" : "available" },
        { "effectiveRTIndirectRays", UsesRTExclusive() ? settings.IndirectRays : 0u },
		{ "rtDirectRequired", settings.UseRTExclusive },
		{ "rtDirectEnabled", direct.loaded && direct.settings.UsePreparedDirectLighting },
		{ "indirectRays", settings.IndirectRays }, { "indirectStrength", settings.IndirectStrength },
		{ "rtTraceResolutionMode", (globals::features::rtComposer.composition.directCheckerboard ? 5u : 0u) },
		{ "connectHybrid", settings.ConnectHybrid },
		{ "trace", globals::features::dx12NativeScene.Tracer().GetDiagnostics() } };
}

void RTIndirectLighting::RegisterUxActions()
{
	FEATURE_COMMAND("setIndirectProducer", "Select rt-exclusive or hybrid GI; dxr/raymarched aliases remain accepted.",
		[](Feature* self, const json& args) {
			if (globals::features::rtComposer.TierControlsLocked())
                throw std::invalid_argument("Fixed composer tier locks indirect producer settings; choose Custom.");
			auto& gi = *static_cast<RTIndirectLighting*>(self);
			const auto producer = args.at("producer").get<std::string>();
			if (producer == "rt-exclusive" || producer == "dxr") {
				if (globals::game::isVR || !globals::features::rtDirectLighting.loaded)
					throw std::invalid_argument("RT-exclusive requires an enabled RT Direct Lighting feature outside VR");
				gi.SelectRTExclusive();
			} else if (producer == "hybrid" || producer == "raymarched") gi.SelectHybrid();
			else throw std::invalid_argument("Producer must be rt-exclusive or hybrid");
			if (args.contains("rays")) gi.settings.IndirectRays = std::clamp(args.at("rays").get<uint32_t>(), 1u, 8u);
			if (args.contains("traceResolution"))
                throw std::invalid_argument("Independent RT-exclusive resolution is retired; use native directCheckerboard.");
			globals::features::dx12NativeScene.Tracer().Reset();
		});
	FEATURE_COMMAND("setLightingSampling", "Adjust existing Hybrid GI slice counts.",
		[](Feature*, const json& args) {
			if (globals::features::rtComposer.TierControlsLocked())
                throw std::invalid_argument("Fixed composer tier locks GI sampling; choose Custom.");
			auto& gi = globals::features::redstoneGI;
			const auto current = gi.GetLightingSamples();
			gi.SetLightingSamples(args.value("diffuseSlices", current[0]), args.value("specularSlices", current[1]));
		});
}

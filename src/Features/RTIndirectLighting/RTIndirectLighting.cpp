#include "RTIndirectLighting.h"
#include "Globals.h"
#include "Features/RTDirectLighting/RTDirectLighting.h"
#include "Features/RedstoneGI/RedstoneGI.h"
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
	selected.IndirectStrength = std::isfinite(selected.IndirectStrength) ? std::clamp(selected.IndirectStrength, 0.0f, 8.0f) : 1.0f;
	settings = selected;
	if (settings.UseRTExclusive) {
		// Keep an old RT-exclusive selection valid after migration.
		globals::features::rtDirectLighting.settings.UsePreparedDirectLighting = true;
		globals::features::dx12NativeScene.RequestPreparedScene();
	} else {
		globals::features::redstoneGI.SelectRenderer(true);
	}
}

void RTIndirectLighting::SaveSettings(json& value) { value = settings; }
void RTIndirectLighting::RestoreDefaultSettings()
{
	settings = {};
	globals::features::redstoneGI.SelectRenderer(true);
}

bool RTIndirectLighting::UsesRTExclusive() const
{
	const auto& direct = globals::features::rtDirectLighting;
	return loaded && settings.UseRTExclusive && !globals::game::isVR &&
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
	globals::features::redstoneGI.SelectRenderer(true);
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
		ImGui::TextWrapped("RT-exclusive uses the prepared native scene for indirect diffuse and specular. RT Direct Lighting is required and both contributions share one GPU trace.");
		int rays = int(settings.IndirectRays);
		if (ImGui::SliderInt("Indirect rays per pixel", &rays, 1, 8)) {
			settings.IndirectRays = uint32_t(rays);
			globals::features::dx12NativeScene.Tracer().Reset();
		}
		ImGui::SliderFloat("Indirect intensity", &settings.IndirectStrength, 0.0f, 8.0f, "%.2fx");
		const auto trace = globals::features::dx12NativeScene.Tracer().GetDiagnostics();
		if (!trace.value("bakedRoomsKnown", false))
			ImGui::TextWrapped("Prepared room data is not ready. Regenerate the cell cache if it predates room metadata.");
		const auto failure = trace.value("failure", std::string{});
		if (!failure.empty()) ImGui::TextWrapped("RT-exclusive lighting unavailable: %s", failure.c_str());
	} else {
		ImGui::TextWrapped("Hybrid uses raymarched indirect diffuse/specular and occlusion with the raster baseline, optionally combined with RT Direct Lighting.");
		if (globals::features::rtDirectLighting.loaded && globals::features::rtDirectLighting.settings.UsePreparedDirectLighting) {
			ImGui::Checkbox("Connect Hybrid GI to prepared direct lighting", &settings.ConnectHybrid);
			ImGui::SliderFloat("Prepared indirect intensity", &settings.IndirectStrength, 0.0f, 8.0f, "%.2fx");
		}
		globals::features::redstoneGI.DrawSettings();
	}
}

json RTIndirectLighting::GetDiagnostics()
{
	const auto& direct = globals::features::rtDirectLighting;
	return { { "mode", settings.UseRTExclusive ? "rt-exclusive" : "hybrid" },
		{ "rtExclusiveActive", UsesRTExclusive() },
		{ "rtDirectRequired", settings.UseRTExclusive },
		{ "rtDirectEnabled", direct.loaded && direct.settings.UsePreparedDirectLighting },
		{ "indirectRays", settings.IndirectRays }, { "indirectStrength", settings.IndirectStrength },
		{ "connectHybrid", settings.ConnectHybrid },
		{ "trace", globals::features::dx12NativeScene.Tracer().GetDiagnostics() } };
}

void RTIndirectLighting::RegisterUxActions()
{
	FEATURE_COMMAND("setIndirectProducer", "Select rt-exclusive or hybrid GI; dxr/raymarched aliases remain accepted.",
		[](Feature* self, const json& args) {
			auto& gi = *static_cast<RTIndirectLighting*>(self);
			const auto producer = args.at("producer").get<std::string>();
			if (producer == "rt-exclusive" || producer == "dxr") {
				if (globals::game::isVR || !globals::features::rtDirectLighting.loaded)
					throw std::invalid_argument("RT-exclusive requires an enabled RT Direct Lighting feature outside VR");
				gi.SelectRTExclusive();
			} else if (producer == "hybrid" || producer == "raymarched") gi.SelectHybrid();
			else throw std::invalid_argument("Producer must be rt-exclusive or hybrid");
			if (args.contains("rays")) gi.settings.IndirectRays = std::clamp(args.at("rays").get<uint32_t>(), 1u, 8u);
		});
	FEATURE_COMMAND("setLightingSampling", "Adjust existing Hybrid GI slice counts.",
		[](Feature*, const json& args) {
			auto& gi = globals::features::redstoneGI;
			const auto current = gi.GetLightingSamples();
			gi.SetLightingSamples(args.value("diffuseSlices", current[0]), args.value("specularSlices", current[1]));
		});
}

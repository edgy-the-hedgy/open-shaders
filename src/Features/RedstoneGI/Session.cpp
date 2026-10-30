#include "RedstoneGI.h"
#include "Deferred.h"
#include "Utils/UI.h"
#include "Utils/Game.h"
#include "Utils/DevBenchUx.h"

void RedstoneGI::ResetRuntimeHistory()
{
	QueueHistoryReset();
	geometryHistoryValid = rrHistoryValid = rrPreviewValid = rrLightingReady = rrProviderSucceeded = rrOutputApplied = false;
}

void RedstoneGI::ImportLegacySettings(json& a_root)
{
	if (a_root.contains(GetName())) return;
	const auto legacy = a_root.find("Upscaling");
	if (legacy == a_root.end() || !legacy->is_object()) return;
	auto migrated = legacy->value("redstoneGI", DefaultRedstoneSettings());
	if (!migrated.is_object()) migrated = DefaultRedstoneSettings();
	migrated["Selected"] = legacy->value("fsrRedstoneEnabled", false);
	a_root[GetName()] = std::move(migrated);
}

void RedstoneGI::OnWorldRenderBegin()
{
	if (compositionSettings && recompileFlag) ClearShaderCache();
}

int RedstoneGI::GetIndirectLightingPriority() const
{
	return selected && resourcesReady && !switchFailed && IsRuntimeReady() ? 100 : -1;
}

void RedstoneGI::OnIndirectLightingSelection(bool a_selected)
{
	if (auto* setting = RE::GetINISetting("bSAOEnable:Display")) {
		if (a_selected) previousVanillaSSAO = setting->data.b;
		else if (previousVanillaSSAO) {
			setting->data.b = *previousVanillaSSAO;
			auto imageSpaceManager = globals::game::imageSpaceManager;
			GET_INSTANCE_MEMBER_VRPTR(BSImagespaceShaderISSAOBlurH, imageSpaceManager);
			if (auto* sao = BSImagespaceShaderISSAOBlurH) sao->enableSAO = *previousVanillaSSAO;
			previousVanillaSSAO.reset();
		}
	}
	active.store(a_selected);
	ResetRuntimeHistory();
	SetRROutputOnly(false);
	SetRRInputsOnly(false);
}

void RedstoneGI::DrawRendererSettings()
{
	ImGui::TextUnformatted("GI Method");
	if (ImGui::RadioButton("SSGI", !selected)) selected = false;
	ImGui::SameLine();
	{
		Util::DisableGuard disabled(!resourcesReady || switchFailed);
		if (ImGui::RadioButton("RedstoneGI", selected)) selected = true;
	}
	auto* provider = globals::deferred->GetIndirectLightingProvider();
	ImGui::Text("Active renderer: %s", provider ? provider->GetDisplayName().c_str() : "Native lighting");
	if (!resourcesReady)
		ImGui::TextWrapped("RGI requires a supported non-VR FSR Frame Generation session and ready shaders/resources. Available native lighting providers remain usable.");
	else if (switchFailed)
		ImGui::TextWrapped("RGI compositor failed. Reload shaders to retry. Available native lighting providers remain usable.");
}

void RedstoneGI::DrawOverlay()
{
	if (active.load() && IsRRDebugVisible()) DrawRRDebugOverlay();
}

bool RedstoneGI::IsOverlayVisible() const
{
	return active.load() && IsRRDebugVisible();
}

json RedstoneGI::GetRuntimeFlags()
{
	return { { "RROutputOnly", IsRROutputOnly() }, { "RRInputsOnly", IsRRInputsOnly() } };
}

bool RedstoneGI::SetRuntimeFlag(std::string_view a_name, bool a_value)
{
	if (a_name == "RROutputOnly") SetRROutputOnly(a_value);
	else if (a_name == "RRInputsOnly") SetRRInputsOnly(a_value);
	else return false;
	return true;
}

json RedstoneGI::GetRedstoneStatus() const
{
	auto* provider = globals::deferred->GetIndirectLightingProvider();
	json saved;
	SaveRedstoneSettings(saved);
	return {
		{ "eligibilityChecked", startupGate.checked }, { "eligible", startupGate.eligible },
		{ "selected", selected }, { "active", active.load() },
		{ "runtimeReady", resourcesReady && !switchFailed && IsRuntimeReady() },
		{ "activeGI", provider ? provider->GetShortName() : "Native" },
		{ "settings", saved }, { "redstoneGI", const_cast<RedstoneGI*>(this)->GetDiagnostics() }
	};
}

void RedstoneGI::RegisterUxActions()
{
	FEATURE_QUERY("redstoneStatus",
		"Return cached FSR-Redstone startup gates, independent RGI settings, engine context capacity, active RR dispatch size, independent IL and occlusion RR signals, packed occlusion layout, checkerboard layout and compositor consumption. Runtime flags RROutputOnly and RRInputsOnly select mutually exclusive previews; setRedstoneDebugView selects combined/diffuse/specular/diffuseAO/specularOcclusion/combinedOcclusion/allSignals/geometryDepth and inputs/outputs. Geometry depth shows native working depth with fixed zero white, frame-maximum black and invalid pixels red, independent of RR success. IL previews apply scene gains and display exposure; occlusion previews show raw grayscale visibility without gains. Params: none.",
		[](const Feature* a_self, const json&) { return static_cast<const RedstoneGI*>(a_self)->GetRedstoneStatus(); });
	FEATURE_COMMAND("setRedstoneDebugView",
		"Select and enable a temporary RR preview without changing saved settings or scene contributions. Params: view (required string: combined, diffuse, specular, diffuseAO, specularOcclusion, combinedOcclusion, allSignals or geometryDepth), source (optional string: inputs or outputs; default outputs). IL previews apply scene gains and preview exposure. Occlusion previews show raw grayscale visibility (0 blocked, 1 exposed), require the selected AO control enabled and ignore strength/exposure. With FSR-RR off, output views show raw resolved signals and are labeled bypassed. Combined occlusion is R/G; allSignals is a labeled 2x2 grid. Checkerboard input gaps remain black. geometryDepth instead displays native working linear depth: zero is fixed white, the farthest valid surface in the current frame is black, and missing/invalid depth is red. It ignores source, checkerboard and exposure and does not require RR success.",
		[](Feature* a_self, const json& a_args) {
			auto* redstone = static_cast<RedstoneGI*>(a_self);
			const auto view = a_args.at("view").get<std::string>();
			RedstoneGI::RRDebugView selected;
			if (view == "combined")
				selected = RedstoneGI::RRDebugView::Combined;
			else if (view == "diffuse")
				selected = RedstoneGI::RRDebugView::Diffuse;
			else if (view == "specular")
				selected = RedstoneGI::RRDebugView::Specular;
			else if (view == "diffuseAO")
				selected = RedstoneGI::RRDebugView::DiffuseOcclusion;
			else if (view == "specularOcclusion")
				selected = RedstoneGI::RRDebugView::SpecularOcclusion;
			else if (view == "combinedOcclusion")
				selected = RedstoneGI::RRDebugView::CombinedOcclusion;
			else if (view == "allSignals")
				selected = RedstoneGI::RRDebugView::AllSignals;
			else if (view == "geometryDepth")
				selected = RedstoneGI::RRDebugView::GeometryDepth;
			else
				throw std::runtime_error("Unknown RR debug view");
			const auto source = a_args.value("source", std::string("outputs"));
			if (source != "inputs" && source != "outputs")
				throw std::runtime_error("RR debug source must be inputs or outputs");
			redstone->SetRRDebugView(selected);
			if (source == "inputs")
				redstone->SetRRInputsOnly(true);
			else
				redstone->SetRROutputOnly(true);
		});
	FEATURE_COMMAND("setRedstoneCheckerboard",
		"Set RR checkerboard sampling for the active Redstone session and reset its history. Saved by the normal settings Save action; baseline SSGI is unchanged. Params: enabled (boolean, required).",
		[](Feature* a_self, const json& a_args) {
			auto* redstone = static_cast<RedstoneGI*>(a_self);
			redstone->SetRRCheckerboard(a_args.at("enabled").get<bool>());
		});
	FEATURE_COMMAND("setRedstoneRenderer",
		"Select the live GI renderer. Params: renderer (required: ssgi or rgi). RGI requires ready resources in an eligible non-VR FSR-FG session. Switches at the next world-render boundary, resets GI histories and preserves both configurations. Only the selected renderer runs. RedstoneGI owns its registered page and settings; original SSGI remains independently usable. Normal Save persists selection. Query redstoneStatus for requested and actual state.",
		[](Feature* a_self, const json& a_args) {
			auto* redstone = static_cast<RedstoneGI*>(a_self);
			const auto renderer = a_args.at("renderer").get<std::string>();
			if (renderer != "ssgi" && renderer != "rgi") throw std::invalid_argument("renderer must be ssgi or rgi");
			if (renderer == "rgi" && (!redstone->resourcesReady || redstone->switchFailed))
				throw std::runtime_error("RGI resources or compositor are unavailable");
			redstone->SelectRenderer(renderer == "rgi");
		});
	FEATURE_COMMAND("setRedstoneDenoiserConfiguration",
		"Set shared SDK 2.3.0 RR configuration overrides for all active RR contexts. Optional params: normalStrength, stabilityBias, maxRadiance, radianceClipStdK, gaussianRelaxation, disocclusionThreshold, debugDepthMin, debugDepthMax (finite numbers or null to restore provider default); reset (boolean) clears all overrides first. Omitted keys remain unchanged. Debug bounds must be nonnegative and ordered and affect only SDK depth visualization. Applies before the next RR dispatch and resets history. Normal Save persists under Upscaling.FSRRayRegeneration.configuration. Read provider defaults, overrides and pending status through getRedstoneStatus.",
		[](Feature* a_self, const json& a_args) {
			auto* redstone = static_cast<RedstoneGI*>(a_self);
			auto& denoiser = redstone->GetDenoiser();
			auto selected = a_args.value("reset", false) ? json::object() : RayRegeneration::GetConfiguration().overrides;
			for (const auto* key : { "normalStrength", "stabilityBias", "maxRadiance", "radianceClipStdK", "gaussianRelaxation", "disocclusionThreshold", "debugDepthMin", "debugDepthMax" })
				if (a_args.contains(key)) selected[key] = a_args.at(key);
			denoiser.SetConfiguration(selected);
		});
	FEATURE_COMMAND("setRedstoneSampling",
		"Set independent direct-RGB RGI sampling and diagnostics. Optional params: diffuseSlices/specularSlices (1..10), diffuseSteps/specularSteps (1..20), adaptiveSampling (boolean), vanillaSSAO (boolean), roughnessFloor (0.02..1; default 0.2), occluderDepth (1..300 world units; default 300; shared by all four signals). Each estimator defaults to 2 slices and 10 steps. Vanilla SSAO is independent of both lighting signals. Saved by normal Save; baseline SSGI stays separate.",
		[](Feature* a_self, const json& a_args) {
			auto* redstone = static_cast<RedstoneGI*>(a_self);
			json selected;
			redstone->SaveRedstoneSettings(selected);
			for (const auto& [parameter, setting] : { std::pair{ "diffuseSlices", "DiffuseSlices" },
					 std::pair{ "diffuseSteps", "DiffuseSteps" }, std::pair{ "specularSlices", "SpecularSlices" },
					 std::pair{ "specularSteps", "SpecularSteps" } }) {
				if (a_args.contains(parameter))
					selected[setting] = std::clamp(a_args.at(parameter).get<int>(), 1, std::string_view(parameter).ends_with("Slices") ? 10 : 20);
			}
			if (a_args.contains("resolutionMode")) {
				const auto rr = RayRegeneration::GetSettings();
				RayRegeneration::Configure(a_args.at("resolutionMode").get<uint32_t>(), rr.checkerboard, rr.enabled);
			}
			if (a_args.contains("adaptiveSampling")) selected["EnableAdaptiveSampling"] = a_args.at("adaptiveSampling").get<bool>();
			if (a_args.contains("vanillaSSAO")) selected["EnableVanillaSSAO"] = a_args.at("vanillaSSAO").get<bool>();
			if (a_args.contains("roughnessFloor")) selected["SpecularRoughnessFloor"] = a_args.at("roughnessFloor").get<float>();
			if (a_args.contains("occluderDepth")) selected["OccluderDepth"] = a_args.at("occluderDepth").get<float>();
			redstone->LoadRedstoneSettings(selected);
		});
	FEATURE_COMMAND("setRedstoneSignals",
		"Set independently saved Redstone signal toggles. Optional booleans: rrEnabled (default true; false uses raw resolve), diffuseEnabled/specularEnabled (default true; stop IL estimators and contributions), diffuseOcclusionEnabled/specularOcclusionEnabled (default false). Omitted values remain unchanged. Resets history; does not change startup FSR-FG/hardware gates or baseline SSGI.",
		[](Feature* a_self, const json& a_args) {
			auto* redstone = static_cast<RedstoneGI*>(a_self);
			json selected;
			redstone->SaveRedstoneSettings(selected);
			if (a_args.contains("rrEnabled")) {
				const auto rr = RayRegeneration::GetSettings();
				RayRegeneration::Configure(rr.resolutionMode, rr.checkerboard, a_args.at("rrEnabled").get<bool>());
			}
			for (const auto& [parameter, setting] : {
				std::pair{ "diffuseEnabled", "DiffuseEnabled" }, std::pair{ "specularEnabled", "SpecularEnabled" },
				std::pair{ "diffuseOcclusionEnabled", "DiffuseOcclusionEnabled" }, std::pair{ "specularOcclusionEnabled", "SpecularOcclusionEnabled" } }) {
				if (a_args.contains(parameter)) selected[setting] = a_args.at(parameter).get<bool>();
			}
			redstone->LoadRedstoneSettings(selected);
		});
	FEATURE_COMMAND("setRedstoneOcclusion",
		"Set independent RGI occlusion controls without changing baseline SSGI or IL budgets. Optional params: diffuseEnabled/specularEnabled (boolean, default false), diffuseSlices/specularSlices (1..10, default 2), diffuseSteps/specularSteps (1..20, default 10), diffuseRadius/specularRadius (16..8192 world units, default 256), diffuseStrength/specularStrength (0..1, default 1). Signals are denoised separately, then packed as diffuse R/specular G; visibility 1 is neutral. Only estimated native ambient and environment specular are attenuated. Saved by normal Save. Params omitted remain unchanged.",
		[](Feature* a_self, const json& a_args) {
			auto* redstone = static_cast<RedstoneGI*>(a_self);
			json selected;
			redstone->SaveRedstoneSettings(selected);
			for (const auto& [parameter, setting] : {
				std::pair{ "diffuseEnabled", "DiffuseOcclusionEnabled" }, std::pair{ "specularEnabled", "SpecularOcclusionEnabled" },
				std::pair{ "diffuseSlices", "DiffuseOcclusionSlices" }, std::pair{ "specularSlices", "SpecularOcclusionSlices" },
				std::pair{ "diffuseSteps", "DiffuseOcclusionSteps" }, std::pair{ "specularSteps", "SpecularOcclusionSteps" },
				std::pair{ "diffuseRadius", "DiffuseOcclusionRadius" }, std::pair{ "specularRadius", "SpecularOcclusionRadius" },
				std::pair{ "diffuseStrength", "DiffuseOcclusionStrength" }, std::pair{ "specularStrength", "SpecularOcclusionStrength" } }) {
				if (a_args.contains(parameter)) selected[setting] = a_args.at(parameter);
			}
			redstone->LoadRedstoneSettings(selected);
		});
	FEATURE_COMMAND("setRedstoneContributions",
		"Set active RGI lighting contribution in EV (0=unchanged, +1=double), clamped to [-8,8]. Affects scene composition and both RR previews, before display-only preview exposure. Saved by the normal Save action; baseline SSGI stays separate. Params: overallEV, specularEV, diffuseEV (optional numbers; omitted values stay unchanged).",
		[](Feature* a_self, const json& a_args) {
			auto* redstone = static_cast<RedstoneGI*>(a_self);
			json selected;
			redstone->SaveRedstoneSettings(selected);
			for (const auto& [parameter, setting] : { std::pair{ "overallEV", "ContributionEV" },
					 std::pair{ "specularEV", "SpecularContributionEV" }, std::pair{ "diffuseEV", "DiffuseContributionEV" } }) {
				if (a_args.contains(parameter))
					selected[setting] = a_args.at(parameter).get<float>();
			}
			redstone->LoadRedstoneSettings(selected);
		});
}

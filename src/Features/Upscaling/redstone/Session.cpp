#include "Session.h"
#include "Features/Upscaling.h"
#include "Routing.h"
#include "Deferred.h"
#include "Utils/UI.h"

void Upscaling::InitializeRedstoneSession()
{
	if (!redstoneGate.Select(
			GetFrameGenMethod() == FrameGenMethod::kFSR && fidelityFX.frameGenContext && !globals::game::isVR,
			true,
			[&] { return redstoneHardwareCompatible; }))
		return;
	auto& original = globals::features::screenSpaceGI;
	if (!original.loaded)
		return;
	try {
		auto session = std::make_unique<RedstoneSession>();
		if (!session->denoiser.Initialize())
			return;
		auto& replacement = session->gi;
		replacement.LoadRedstoneSettings(redstoneGISettings);
		replacement.loaded = original.loaded;
		replacement.installed = original.installed;
		replacement.version = original.version;
		replacement.failedLoadedMessage = original.failedLoadedMessage;
		redstone = std::move(session);
		logger::info("[Redstone] Runtime switching available; original SSGI remains registered");
	} catch (const std::exception& e) {
		logger::error("[Redstone] Startup initialization failed; retaining original SSGI: {}", e.what());
	}
}

namespace
{
	void RestoreSSGIRouting()
	{
		Redstone::getCompositeShader = nullptr;
		Redstone::bindComposite = nullptr;
		Redstone::finishComposite = nullptr;
		Redstone::privateCompositeSelected = false;
		Redstone::activeSSGI = &globals::features::screenSpaceGI;
		Redstone::drawSSGI = &ScreenSpaceGI::DrawSSGI;
	}
}

void RedstoneGI::ResetRuntimeHistory()
{
	QueueHistoryReset();
	rrDepthHistoryValid = false;
	rrPreviewValid = false;
	rrLightingReady = false;
	rrProviderSucceeded = false;
	rrOutputApplied = false;
}

void Upscaling::SetupRedstoneResources()
{
	RestoreSSGIRouting();
	redstoneActive.store(false);
	redstoneResourcesReady = false;
	redstoneSwitchFailed = false;
	if (!redstone) return;
	try {
		redstone->gi.SetupResources();
		redstone->gi.ResetRuntimeHistory();
		redstoneResourcesReady = redstone->gi.IsRuntimeReady();
	} catch (const std::exception& e) {
		logger::error("[Redstone] Resource setup failed; retaining SSGI: {}", e.what());
	}
}

void Upscaling::OnWorldRenderBegin()
{
	const bool selected = settings.fsrRedstoneEnabled && redstone && redstoneResourcesReady && !redstoneSwitchFailed && redstone->gi.IsRuntimeReady();
	if (selected == redstoneActive.load()) return;
	RestoreSSGIRouting();
	globals::features::screenSpaceGI.QueueHistoryReset();
	redstone->gi.ResetRuntimeHistory();
	redstone->gi.SetRROutputOnly(false);
	redstone->gi.SetRRInputsOnly(false);
	if (selected) {
		Redstone::activeSSGI = &redstone->gi;
		Redstone::drawSSGI = static_cast<void (ScreenSpaceGI::*)()>(&RedstoneGI::DrawGI);
		Redstone::getCompositeShader = [](bool interior, std::vector<std::pair<const char*, const char*>>& defines) {
			return globals::features::upscaling.redstone->gi.GetCompositeShader(interior, defines);
		};
		Redstone::bindComposite = [] { globals::features::upscaling.redstone->gi.BindCompositeResources(); };
		Redstone::finishComposite = [](bool used) { globals::features::upscaling.redstone->gi.FinishComposite(used); };
		globals::deferred->GetComputeMainComposite();
		bool ready = Redstone::privateCompositeSelected;
		globals::deferred->GetComputeMainCompositeInterior();
		ready &= Redstone::privateCompositeSelected;
		if (!ready) {
			RestoreSSGIRouting();
			globals::deferred->ClearShaderCache();
			redstoneSwitchFailed = true;
			logger::error("[Redstone] RGI compositor unavailable; retaining SSGI until shader reload");
			return;
		}
	}
	redstoneActive.store(selected);
	logger::info("[Redstone] Active renderer: {}", selected ? "RGI" : "SSGI");
}

void Upscaling::OnSceneTransitionReset(bool)
{
	neuralRendering.ResetHistory();
	if (redstone) redstone->gi.ResetRuntimeHistory();
}

std::vector<Feature*> Upscaling::GetAdditionalMenuPages()
{
	return redstone && redstoneActive.load() ? std::vector<Feature*>{ &redstone->gi } : std::vector<Feature*>{};
}

void Upscaling::DrawRedstoneSettings()
{
	ImGui::TextUnformatted("GI Method");
	if (ImGui::RadioButton("SSGI", !settings.fsrRedstoneEnabled)) settings.fsrRedstoneEnabled = false;
	ImGui::SameLine();
	{
		Util::DisableGuard disabled(!redstoneResourcesReady || redstoneSwitchFailed);
		if (ImGui::RadioButton("RedstoneGI", settings.fsrRedstoneEnabled)) settings.fsrRedstoneEnabled = true;
	}
	ImGui::Text("Active renderer: %s", redstoneActive.load() ? "RGI" : "SSGI");
	if (!redstoneResourcesReady)
		ImGui::TextWrapped("RGI is unavailable. It requires a supported non-VR FSR Frame Generation session, supported hardware and ready shaders/resources. SSGI remains available.");
	else if (redstoneSwitchFailed)
		ImGui::TextWrapped("RGI compositor failed to compile. SSGI remains active; reload shaders to retry.");
	else
		ImGui::TextWrapped("Changes apply at the next world-render boundary without restarting. When RGI is active, its settings page appears directly below Upscaling. Save settings to remember your selection.");
}

void Upscaling::SaveRedstoneSettings(json& a_json) const
{
	if (redstone)
		redstone->gi.SaveRedstoneSettings(a_json);
	else
		a_json = redstoneGISettings;
}

void Upscaling::LoadRedstoneSettings(const json& a_json)
{
	redstoneGISettings = a_json.is_object() ? a_json : RedstoneGI::DefaultRedstoneSettings();
	if (redstone)
		redstone->gi.LoadRedstoneSettings(redstoneGISettings);
}

void Upscaling::DrawOverlay()
{
	neuralRendering.DrawDiagnosticsOverlay();
	if (redstoneActive.load() && redstone && redstone->gi.IsRRDebugVisible())
		redstone->gi.DrawRRDebugOverlay();
}

bool Upscaling::IsOverlayVisible() const
{
	return neuralRendering.DiagnosticsOverlayVisible() || (redstoneActive.load() && redstone && redstone->gi.IsRRDebugVisible());
}

json Upscaling::GetRuntimeFlags()
{
	return { { "RROutputOnly", redstoneActive.load() && redstone && redstone->gi.IsRROutputOnly() },
		{ "RRInputsOnly", redstoneActive.load() && redstone && redstone->gi.IsRRInputsOnly() } };
}

bool Upscaling::SetRuntimeFlag(std::string_view a_name, bool a_value)
{
	if (!redstone || !redstoneActive.load())
		return false;
	if (a_name == "RROutputOnly")
		redstone->gi.SetRROutputOnly(a_value);
	else if (a_name == "RRInputsOnly")
		redstone->gi.SetRRInputsOnly(a_value);
	else
		return false;
	return true;
}

json Upscaling::GetRedstoneStatus() const
{
	return {
		{ "hardwareCompatible", redstoneHardwareCompatible },
		{ "tabVisible", redstoneHardwareCompatible },
		{ "eligibilityChecked", redstoneGate.checked },
		{ "eligible", redstoneGate.eligible },
		{ "enabledAtStartup", bootSnapshot.Boot(&Settings::fsrRedstoneEnabled) },
		{ "selected", settings.fsrRedstoneEnabled },
		{ "restartRequired", false },
		{ "active", redstoneActive.load() },
		{ "runtimeReady", redstoneResourcesReady && !redstoneSwitchFailed },
		{ "activeGI", redstoneActive.load() ? "RedstoneGI" : "ScreenSpaceGI" },
		{ "originalSSGIAvailable", globals::features::screenSpaceGI.loaded },
		{ "settings", [&] { json selected; SaveRedstoneSettings(selected); return selected; }() },
		{ "redstoneGI", redstone ? redstone->gi.GetDiagnostics() : json::object() }
	};
}

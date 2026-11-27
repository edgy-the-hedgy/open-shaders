#include "DX12Control.h"
#include "DX12NativeScene.h"
#include "DX12Resources.h"
#include "Features/NativePBR/NativeMaterialMetadata.h"
#include "TruePBR.h"
#include "Globals.h"
#include "State.h"
#include "Utils/DevBenchUx.h"
#include "Utils/UI.h"

#include <algorithm>

DX12::ControlMode DX12Control::GetMode() const
{
	using DX12::ControlMode;
	int mode = selectedMode.load(std::memory_order_relaxed);
	// Check the existing developer gate only for the diagnostic mode.
	if (mode == static_cast<int>(ControlMode::RasterComparison) &&
		(!globals::state || !globals::state->IsDeveloperMode())) {
		const int fallback = static_cast<int>(ControlMode::ShadowOffload);
		if (selectedMode.compare_exchange_strong(mode, fallback, std::memory_order_relaxed))
			mode = fallback;
	}
	return static_cast<ControlMode>(mode);
}

void DX12Control::RequestMode(DX12::ControlMode mode)
{
	settings.Mode = static_cast<int>(mode);
	if (!bootCaptured || !DX12::RequiresModeRestart(GetMode(), mode))
		selectedMode.store(settings.Mode, std::memory_order_relaxed);
}

	
void DX12Control::InitializeMaterialAuthority()
{
	if (authorityCaptured)
		return;
	authorityCaptured = true;
	if (!WantsMaterialPreparation() || globals::game::isVR)
		return;
	NativePBR::InstallExclusiveMetadataHooks();
	exclusiveMaterials.store(true, std::memory_order_release);
	globals::features::truePBR.loaded = false;
	logger::info("[DX12] Full Native owns material discovery and evaluation; TruePBR is disabled for this session. Vanilla raster lighting remains the fallback.");
}

void DX12Control::DrawSimpleSettings(bool developerMode)
{
	using DX12::ControlMode;
	const char* modes[] = {
		T("menu.dx12.mode.shadow_offload", "Shadow offload"),
		T("menu.dx12.mode.ray_traced_static_shadows", "Ray-Traced Static Shadows"),
		T("menu.dx12.mode.full_native", "Full Native (experimental)"),
		T("menu.dx12.mode.raster_comparison", "Raster comparison")
	};
	constexpr ControlMode modeIDs[]{ ControlMode::ShadowOffload, ControlMode::FullRT, ControlMode::FullNative, ControlMode::RasterComparison };
	const auto active = GetMode();
	const auto requested = DX12::SanitizeControlMode(settings.Mode, developerMode);
	const auto selected = DX12::RequiresModeRestart(active, requested) ? requested : active;
	int index = static_cast<int>(std::find(std::begin(modeIDs), std::end(modeIDs), selected) - std::begin(modeIDs));
	settings.Mode = static_cast<int>(selected);
	if (ImGui::Combo(T("menu.dx12.mode.label", "Mode"), &index, modes, developerMode ? int(std::size(modes)) : int(std::size(modes)) - 1)) {
		RequestMode(modeIDs[index]);
	}
	if (DX12::RequiresModeRestart(GetMode(), static_cast<ControlMode>(settings.Mode)))
		ImGui::TextDisabled(T("menu.dx12.mode.restart_full_native", "Entering or leaving Full Native requires a restart."));
	if (IsFullNativeMode())
		ImGui::TextDisabled(T("menu.dx12.mode.full_native_preview", "NativePBR material authority; vanilla raster remains the final-color fallback."));
	ImGui::Checkbox(T("menu.dx12.master.enable", "Enable native DX12 features (restart required)"), &settings.Enabled);
	if (settings.Enabled != bootActive.load(std::memory_order_acquire))
		ImGui::TextDisabled(T("menu.dx12.master.restart_boot_state", "A game restart is required; DX12 Resources retains its boot state until then."));
	if (!IsActive())
		return;

	ImGui::SeparatorText(T("menu.dx12.tab.native_scene", "Native Scene"));
	globals::features::dx12NativeScene.DrawSettings();
	ImGui::SeparatorText(T("menu.dx12.tab.resources", "Resources"));
	globals::features::dx12Resources.DrawSettings();
}

void DX12Control::DrawSettings()
{
	const bool developerMode = globals::state && globals::state->IsDeveloperMode();
	if (!Util::BeginPillTabBar("##DX12Tabs"))
		return;
	if (Util::BeginPillTabItem(T("menu.dx12.tab.resources", "Resources"))) {
		if (IsActive())
			globals::features::dx12Resources.DrawResourceStatus();
		else
			ImGui::TextDisabled("%s", T("menu.dx12.resources.inactive", "DX12 Resources is inactive."));
		Util::EndPillTabItem();
	}
	if (Util::BeginPillTabItem(T("menu.dx12.tab.settings", "Settings"))) {
		DrawSimpleSettings(developerMode);
		Util::EndPillTabItem();
	}
	if (IsActive() && developerMode && Util::BeginPillTabItem(T("menu.dx12.tab.dev", "Dev"))) {
		globals::features::dx12Resources.DrawDevResourceStatus();
		globals::features::dx12NativeScene.DrawDevSettings();
		Util::EndPillTabItem();
	}
	Util::EndPillTabBar();
}

void DX12Control::LoadSettings(json& input)
{
	const Settings defaults{};
	settings.Enabled = input.value("Enabled", defaults.Enabled);
	settings.Mode = static_cast<int>(DX12::SanitizeControlMode(input.value("Mode", defaults.Mode),
		globals::state && globals::state->IsDeveloperMode()));
	RequestMode(static_cast<DX12::ControlMode>(settings.Mode));
	if (!bootCaptured) {
		bootActive.store(settings.Enabled, std::memory_order_release);
		bootCaptured = true;
	}
}

void DX12Control::SaveSettings(json& output)
{
	const auto active = GetMode();
	const auto requested = DX12::SanitizeControlMode(settings.Mode, globals::state && globals::state->IsDeveloperMode());
	const auto saved = DX12::RequiresModeRestart(active, requested) ? requested : active;
	output = { { "Enabled", settings.Enabled }, { "Mode", static_cast<int>(saved) } };
}

void DX12Control::RestoreDefaultSettings()
{
	settings = {};
	RequestMode(static_cast<DX12::ControlMode>(settings.Mode));
	if (!bootCaptured) {
		bootActive.store(settings.Enabled, std::memory_order_release);
		bootCaptured = true;
	}
}

void DX12Control::OnRuntimeDisabled()
{
	if (!bootActive.exchange(false, std::memory_order_acq_rel))
		return;
	globals::features::dx12NativeScene.OnRuntimeDisabled();
	globals::features::dx12Resources.OnRuntimeDisabled();
}

void DX12Control::RegisterUxActions()
{
	FEATURE_COMMAND("setValidationMode", "Developer-only live mode switch for shadow A/B tests. Params: mode (int).",
		[](Feature* self, const json& args) {
			if (!globals::state || !globals::state->IsDeveloperMode())
				return;
			auto* control = static_cast<DX12Control*>(self);
			if (!control->IsActive())
				return;
			const auto mode = DX12::SanitizeControlMode(args.value("mode", 0), true);
			// The validation command remains restricted to the existing live shadow A/B modes.
			if (mode == DX12::ControlMode::FullNative || control->IsFullNativeMode())
				return;
			control->RequestMode(mode);
		});
}

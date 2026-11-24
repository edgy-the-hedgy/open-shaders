#include "DX12Control.h"
#include "DX12NativeScene.h"
#include "DX12Resources.h"
#include "Globals.h"
#include "State.h"
#include "Utils/UI.h"

#include <algorithm>

void DX12Control::DrawSettings()
{
	ImGui::Checkbox(T("menu.dx12.master.enable", "Enable DX12 (restart required)"), &settings.Enabled);
	const bool active = IsActive();
	if (settings.Enabled != bootActive.load(std::memory_order_acquire))
		ImGui::TextWrapped("%s", T("menu.dx12.master.restart_boot_state", "Restart Skyrim to apply the DX12 enable setting."));

	ImGui::SeparatorText(T("menu.dx12.tab.resources", "Resources"));
	const auto drawResourceControls = [&]() {
		ImGui::TextUnformatted(T("menu.dx12.mode.label", "Rendering mode"));
		const char* modes[] = { T("menu.dx12.mode.shadow_offload", "Shadow Offloading"), T("menu.dx12.mode.raster_comparison", "Raster Comparison"), T("menu.dx12.mode.full_native", "Full Native") };
		int mode = settings.Mode;
		for (int i = 0; i < int(std::size(modes)); ++i)
			if (ImGui::RadioButton(modes[i], &mode, i)) {
				settings.Mode = mode;
				ApplyModeRequest();
			}

		ImGui::TextWrapped("%s", settings.Mode == 0 ?
			T("menu.dx12.mode.shadow_offload_description", "DX12 shadows replace matched raster static shadows.") :
			settings.Mode == 1 ? T("menu.dx12.mode.raster_comparison_description", "Raster shadows remain unchanged; DX12 shadow dispatch is paused for comparison.") :
			T("menu.dx12.mode.full_native_description", "Full resource preparation and experimental material evaluation; raster rendering remains active."));
		if (settings.Mode != selectedMode.load(std::memory_order_relaxed))
			ImGui::TextWrapped("%s", T("menu.dx12.mode.restart_native", "Restart Skyrim to enter or leave Full Native mode."));

		ImGui::Separator();
		if (active)
			globals::features::dx12Resources.DrawSettings();
		else
			ImGui::TextWrapped("%s", T("menu.dx12.resources.inactive", "Resources are not initialized."));
		if (settings.Mode == 2)
			ImGui::TextWrapped("%s", active && IsFullNativeMode() ?
				T("menu.dx12.resources.cache_automatic", "Persistent cache: automatic") :
				T("menu.dx12.resources.cache_pending", "Persistent cache: available after restart"));
	};

	if (ImGui::BeginTable("##DX12ResourceOverview", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("##Subcells", ImGuiTableColumnFlags_WidthStretch, 0.43f);
		ImGui::TableSetupColumn("##Controls", ImGuiTableColumnFlags_WidthStretch, 0.57f);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		globals::features::dx12Resources.DrawSubcellOverview();
		ImGui::TableSetColumnIndex(1);
		drawResourceControls();
		ImGui::EndTable();
	}

	if (!active)
		return;

	ImGui::Spacing();
	ImGui::Separator();
	const bool developer = globals::state && globals::state->IsDeveloperMode();
	if (!Util::BeginPillTabBar("##DX12Tabs"))
		return;
	if (Util::BeginPillTabItem(T("menu.dx12.tab.shadows", "Shadows"))) {
		globals::features::dx12NativeScene.DrawSettings();
		ImGui::SeparatorText(T("menu.dx12.shadows.reconstruction", "Reconstruction"));
		ImGui::Checkbox(T("menu.dx12.composite.spatial_reconstruction", "Spatial shadow reconstruction"), &settings.SpatialReconstruction);
		spatialReconstruction.store(settings.SpatialReconstruction, std::memory_order_relaxed);
		int gridIndex = int((settings.ReconstructionGridSize - 3u) / 2u);
		ImGui::BeginDisabled(!settings.SpatialReconstruction);
		if (ImGui::Combo(T("menu.dx12.composite.reconstruction_grid", "Reconstruction grid"), &gridIndex,
				"3x3\0"
				"5x5\0"
				"7x7\0"
				"9x9\0")) {
			settings.ReconstructionGridSize = uint32_t(3 + gridIndex * 2);
			reconstructionGrid.store(settings.ReconstructionGridSize, std::memory_order_relaxed);
		}
		ImGui::EndDisabled();
		if (ImGui::SliderFloat(T("menu.dx12.composite.shadow_strength", "Shadow strength"), &settings.ShadowStrength, 0.0f, 4.0f, "%.2f"))
			shadowStrength.store(settings.ShadowStrength, std::memory_order_relaxed);
		if (ImGui::SliderFloat(T("menu.dx12.composite.shadow_blend", "Shadow blend"), &settings.ShadowBlend, 0.0f, 1.0f, "%.2f"))
			shadowBlend.store(settings.ShadowBlend, std::memory_order_relaxed);
		ImGui::TextDisabled(T("menu.dx12.composite.before_upscaling", "Reconstruction and shadow application run before upscaling."));
		Util::EndPillTabItem();
	}
	if (settings.Mode == 2) {
		if (Util::BeginPillTabItem(T("menu.dx12.tab.lighting", "Lighting"))) {
			if (IsFullNativeMode())
				globals::features::dx12NativeScene.DrawLightingSettings(developer);
			else
				ImGui::TextWrapped("%s", T("menu.dx12.mode.restart_native", "Restart Skyrim to enter or leave Full Native mode."));
			Util::EndPillTabItem();
		}
		if (Util::BeginPillTabItem(T("menu.dx12.tab.materials", "Materials"))) {
			if (IsFullNativeMode()) {
				ImGui::Checkbox(T("menu.dx12.materials.exclusive", "Exclusive material authority (restart required)"), &settings.ExclusiveMaterials);
				ImGui::TextWrapped("%s", UsesExclusiveMaterials() ?
					T("menu.dx12.materials.exclusive_active", "DX12 material authority selected; TruePBR rendering disabled.") :
					T("menu.dx12.materials.exclusive_inactive", "TruePBR remains the raster material authority."));
				if (settings.ExclusiveMaterials != UsesExclusiveMaterials())
					ImGui::TextWrapped("%s", T("menu.dx12.materials.exclusive_restart", "Restart Skyrim to apply the material authority setting."));
				globals::features::dx12NativeScene.DrawMaterialSettings(developer);
			} else
				ImGui::TextWrapped("%s", T("menu.dx12.mode.restart_native", "Restart Skyrim to enter or leave Full Native mode."));
			Util::EndPillTabItem();
		}
	}
	if (developer && Util::BeginPillTabItem(T("menu.dx12.tab.dev", "Dev"))) {
		globals::features::dx12Resources.DrawDevStats();
		globals::features::dx12NativeScene.DrawDevStats();
		Util::EndPillTabItem();
	}
	Util::EndPillTabBar();
}

void DX12Control::ApplyModeRequest()
{
	const int active = selectedMode.load(std::memory_order_relaxed);
	// Preserve live switching between the two existing lightweight modes.
	// Crossing the Full Native boundary is a session-level backend change.
	if (!bootCaptured || (active != 2 && settings.Mode != 2))
		selectedMode.store(settings.Mode, std::memory_order_relaxed);
}

void DX12Control::LoadSettings(json& input)
{
	settings.Enabled = input.value("Enabled", false);
	settings.Mode = std::clamp(input.value("Mode", 0), 0, 2);
	settings.ExclusiveMaterials = input.value("ExclusiveMaterials", false);
	settings.SpatialReconstruction = input.value("SpatialReconstruction", true);
	const uint32_t requestedGrid = std::clamp(input.value("ReconstructionGridSize", 5u), 3u, 9u);
	settings.ReconstructionGridSize = requestedGrid - ((requestedGrid & 1u) == 0u ? 1u : 0u);
	settings.ShadowStrength = std::clamp(input.value("ShadowStrength", 1.0f), 0.0f, 4.0f);
	settings.ShadowBlend = std::clamp(input.value("ShadowBlend", 0.70f), 0.0f, 1.0f);
	ApplyModeRequest();
	spatialReconstruction.store(settings.SpatialReconstruction, std::memory_order_relaxed);
	reconstructionGrid.store(settings.ReconstructionGridSize, std::memory_order_relaxed);
	shadowStrength.store(settings.ShadowStrength, std::memory_order_relaxed);
	shadowBlend.store(settings.ShadowBlend, std::memory_order_relaxed);
	if (!bootCaptured) {
		exclusiveMaterials.store(settings.Enabled && settings.Mode == 2 && settings.ExclusiveMaterials && !globals::game::isVR, std::memory_order_release);
		bootActive.store(settings.Enabled, std::memory_order_release);
		bootCaptured = true;
	}
}

void DX12Control::SaveSettings(json& output)
{
	output = { { "Enabled", settings.Enabled }, { "Mode", settings.Mode }, { "ExclusiveMaterials", settings.ExclusiveMaterials },
		{ "SpatialReconstruction", settings.SpatialReconstruction },
		{ "ReconstructionGridSize", settings.ReconstructionGridSize },
		{ "ShadowStrength", settings.ShadowStrength }, { "ShadowBlend", settings.ShadowBlend } };
}

void DX12Control::RestoreDefaultSettings()
{
	settings = {};
	ApplyModeRequest();
	spatialReconstruction.store(settings.SpatialReconstruction, std::memory_order_relaxed);
	reconstructionGrid.store(settings.ReconstructionGridSize, std::memory_order_relaxed);
	shadowStrength.store(settings.ShadowStrength, std::memory_order_relaxed);
	shadowBlend.store(settings.ShadowBlend, std::memory_order_relaxed);
}

void DX12Control::OnRuntimeDisabled()
{
	bootActive.store(false, std::memory_order_release);
}

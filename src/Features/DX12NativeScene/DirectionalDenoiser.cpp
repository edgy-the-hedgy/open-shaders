#include "DirectionalTrace.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/RayRegenerationSettings.h"

namespace DX12Native
{
	void DirectionalTrace::DrawRayRegenerationSettings(bool a_dxrIndirect)
	{
		ImGui::PushID("PreparedRayRegeneration");
		ImGui::Text("Prepared RR submissions: %llu | last frame: %u | budget fallbacks: %llu", directRRDispatches, directRRFrame, directRRBudgetDeferrals);
		ImGui::Text("Reconstruction grid: %u x %u | checkerboard: %s", directRRSize[0], directRRSize[1], directRRCheckerboard ? "on" : "off");
		ImGui::TextWrapped("SDK overrides are shared. Availability, histories and failures are independent for every native signal. GPU input/output surfaces are named for RenderDoc; the legacy DX11 signal viewer does not mirror native RR surfaces.");
		auto contextSettings = [](const char* name, RedstoneDenoiser& denoiser) {
			ImGui::PushID(name);
			if (ImGui::TreeNode(name)) {
				uint32_t index = 0;
				for (const auto& signal : denoiser.GetNativeDiagnostics()) {
					ImGui::Text("Signal %u: initialized %s | configured %s | SDK records %llu", index++, signal.value("initialized", false) ? "yes" : "no",
						signal.value("configured", false) ? "yes" : "no", signal.value("dispatchesRecorded", 0ull));
					const auto failure = signal.value("failure", std::string{});
					if (!failure.empty()) ImGui::TextWrapped("%s", failure.c_str());
				}
				ImGui::Text("Active and retiring SDK memory: %.1f MiB", double(denoiser.GetNativeResidentBytes()) / (1024.0 * 1024.0));
				denoiser.DrawConfigurationSettings();
				ImGui::TreePop();
			}
			ImGui::PopID();
		};
		contextSettings("Direct diffuse / specular", directDenoiser);
		if (a_dxrIndirect) contextSettings("DXR indirect diffuse / specular", indirectDenoiser);
		for (const auto& [signalProducer, status] : nativeRRStatus.items())
			if (status.is_object() && status.contains("unavailableReason")) ImGui::TextWrapped("%s: %s", signalProducer.c_str(), status.at("unavailableReason").get_ref<const std::string&>().c_str());
		ImGui::PopID();
	}

}

#include "DirectionalTrace.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/RayRegenerationSettings.h"

namespace RedstoneShadow
{
	uint32_t DirectionalTrace::DenoiseDirect(Frame& a_frame)
	{
		const auto policy = RayRegeneration::GetSettings();
		if (!a_frame.useDirectRR || !policy.enabled) return 0;
		CS_GPU_PASS("RedstoneShadows::PrepareLightingRR");
		try {
			if (!prepareDirectGuides) prepareDirectGuides.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\RedstoneShadows\\prepareDirectGuides.cs.hlsl", {}, "cs_5_0")));
			if (!packRRSignals) packRRSignals.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\RedstoneShadows\\packRRSignals.cs.hlsl", {}, "cs_5_0")));
			if (!prepareDirectGuides || !packRRSignals) return 0;
			const std::array<uint32_t, 2> size{ std::max(1u, a_frame.size[0] >> policy.resolutionMode), std::max(1u, a_frame.size[1] >> policy.resolutionMode) };
			const bool checkerboard = policy.checkerboard && size[0] >= 2 && (size[0] & 1u) == 0;
			if (directRRSize != size || directRRCheckerboard != checkerboard || !directRRTextures[0]) {
				std::array<std::unique_ptr<Texture2D>, 13> textures;
				static constexpr std::array names{ "Depth", "Motion", "FilteredDirectDiffuse", "FilteredDirectSpecular", "FilteredIndirectDiffuse", "FilteredIndirectSpecular", "InputDirectDiffuse", "InputDirectSpecular", "InputIndirectDiffuse", "InputIndirectSpecular", "NormalRoughness", "SpecularAlbedo", "DiffuseAlbedo" };
				for (uint32_t i = 0; i < textures.size(); ++i) {
					D3D11_TEXTURE2D_DESC desc{};
					desc.Width = (i >= 6 && i <= 9 && checkerboard) ? size[0] / 2 : size[0]; desc.Height = size[1];
					desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
					desc.Format = i == 0 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
					desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
					const auto name = std::format("RedstoneShadows::RR{}", names[i]);
					textures[i] = std::make_unique<Texture2D>(desc, name.c_str());
					D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
					srv.Format = desc.Format; srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
					D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
					uav.Format = desc.Format; uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
					textures[i]->CreateSRV(srv); textures[i]->CreateUAV(uav);
				}
				directRRTextures = std::move(textures);
				directRRSize = size;
				directRRCheckerboard = checkerboard;
				directRRGeneration = UINT64_MAX;
			}
			auto* context = backend.d3d11Context.get();
			const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
			struct Grid { std::array<uint32_t, 2> nativeSize, size; uint32_t checkerboard, phase, padding[2]{}; };
			if (!rrGridSettings) rrGridSettings = std::make_unique<ConstantBuffer>(ConstantBufferDesc<Grid>(), "RedstoneShadows::RRGrid CB");
			const uint32_t phase = globals::game::frameCounter ? uint32_t(*globals::game::frameCounter) & 1u : 0;
			rrGridSettings->Update(Grid{ a_frame.size, size, checkerboard ? 1u : 0u, phase });
			auto* cb = rrGridSettings->CB();
			context->CSSetConstantBuffers(0, 1, &cb);
			ID3D11ShaderResourceView* guideInputs[]{ a_frame.output->srv, a_frame.receivers->srv, Util::AsReal(targets[RE::RENDER_TARGET::kMOTION_VECTOR].SRV) };
			if (!guideInputs[2]) return 0;
			ID3D11UnorderedAccessView* guideOutputs[]{ directRRTextures[0]->uav.get(), directRRTextures[1]->uav.get() };
			context->CSSetShaderResources(0, 3, guideInputs);
			context->CSSetUnorderedAccessViews(0, 2, guideOutputs, nullptr);
			context->CSSetShader(prepareDirectGuides.get(), nullptr, 0);
			context->Dispatch((size[0] + 7) / 8, (size[1] + 7) / 8, 1);
			ID3D11UnorderedAccessView* clearedOutputs[7]{};
			context->CSSetUnorderedAccessViews(0, 7, clearedOutputs, nullptr);
			ID3D11ShaderResourceView* inputs[]{ a_frame.output->srv, a_frame.directSignals[0]->srv, a_frame.directSignals[1]->srv, a_frame.directSignals[2]->srv, a_frame.directSignals[3]->srv, a_frame.directSignals[4]->srv, a_frame.directSignals[5]->srv, a_frame.directSignals[6]->srv };
			ID3D11UnorderedAccessView* outputs[]{ directRRTextures[6]->uav.get(), directRRTextures[7]->uav.get(), directRRTextures[8]->uav.get(), directRRTextures[9]->uav.get(), directRRTextures[10]->uav.get(), directRRTextures[11]->uav.get(), directRRTextures[12]->uav.get() };
			context->CSSetShaderResources(0, 8, inputs);
			context->CSSetUnorderedAccessViews(0, 7, outputs, nullptr);
			context->CSSetShader(packRRSignals.get(), nullptr, 0);
			context->Dispatch((size[0] + 7) / 8, (size[1] + 7) / 8, 1);
			ID3D11ShaderResourceView* clearedInputs[8]{};
			context->CSSetShaderResources(0, 8, clearedInputs);
			context->CSSetUnorderedAccessViews(0, 7, clearedOutputs, nullptr);
			directRRPhase = phase; rrPackFrame = a_frame.number;
			const bool reset = directRRGeneration != a_frame.generation || directRRFrame + 1u != a_frame.number || directRRPolicyRevision != policy.revision || directRRIndirectRays != a_frame.indirectRays ||
				std::memcmp(&directRRStrengths, &a_frame.strengths, sizeof(float4)) != 0 || directRRIdentities != a_frame.lightIdentities;
			auto dispatch = [&](RedstoneDenoiser& denoiser, bool direct) {
				if (!denoiser.Initialize(backend)) return false;
				return denoiser.DispatchRayRegeneration(directRRTextures[direct ? 7 : 9]->resource.get(), directRRTextures[0]->resource.get(), directRRTextures[1]->resource.get(), directRRTextures[10]->resource.get(), directRRTextures[11]->resource.get(), directRRTextures[12]->resource.get(), directRRTextures[direct ? 3 : 5]->resource.get(), directRRTextures[direct ? 6 : 8]->resource.get(), directRRTextures[direct ? 2 : 4]->resource.get(), {}, a_frame.size, size[0], size[1], reset, checkerboard, phase, direct);
			};
			uint32_t flags = dispatch(directDenoiser, true) ? 1u : 0u;
			if (a_frame.indirectRays && dispatch(indirectDenoiser, false)) flags |= 2u;
			if (flags) {
				directRRGeneration = flags == (a_frame.indirectRays ? 3u : 1u) ? a_frame.generation : UINT64_MAX;
				directRRFrame = a_frame.number; directRRPolicyRevision = policy.revision; directRRIndirectRays = a_frame.indirectRays;
				directRRStrengths = a_frame.strengths; directRRIdentities = a_frame.lightIdentities;
				++directRRDispatches;
			}
			return flags;
		} catch (const std::exception& error) {
			logger::warn("[RedstoneShadows] Lighting RR unavailable; retaining raw lighting: {}", error.what());
			return 0;
		}
	}

	void DirectionalTrace::DrawRayRegenerationSettings(bool a_dxrIndirect)
	{
		ImGui::PushID("PreparedRayRegeneration");
		ImGui::Text("Prepared RR submissions: %llu | last frame: %u | budget fallbacks: %llu", directRRDispatches, directRRFrame, directRRBudgetDeferrals);
		ImGui::Text("Reconstruction grid: %u x %u | checkerboard: %s", directRRSize[0], directRRSize[1], directRRCheckerboard ? "on" : "off");
		ImGui::TextWrapped("SDK overrides are shared across active RR contexts. Buffers below show the last submitted data and may be older than the current frame.");
		auto contextSettings = [](const char* name, RedstoneDenoiser& denoiser) {
			ImGui::PushID(name);
			if (ImGui::TreeNode(name)) {
				const auto status = denoiser.GetRayRegenerationDiagnostics();
				ImGui::Text("Successful dispatches: %llu | failed: %llu", status.value("successfulDispatches", 0ull), status.value("failedDispatches", 0ull));
				ImGui::Text("RR dispatch: %u x %u | input storage: %u x %u", status.value("dispatchWidth", 0u), status.value("dispatchHeight", 0u), status.value("signalWidth", 0u), status.value("dispatchHeight", 0u));
				ImGui::Text("Context capacity: %u x %u", status.value("contextWidth", 0u), status.value("contextHeight", 0u));
				if (status.value("failureLatched", false)) ImGui::TextUnformatted("Provider unavailable; raw lighting is used.");
				denoiser.DrawConfigurationSettings();
				ImGui::TreePop();
			}
			ImGui::PopID();
		};
		contextSettings("Direct diffuse / specular", directDenoiser);
		if (a_dxrIndirect) contextSettings("DXR indirect diffuse / specular", indirectDenoiser);
		bool inputsOnly = rrFullscreenSource == 1, outputsOnly = rrFullscreenSource == 2;
		if (ImGui::Checkbox("Prepared RR inputs only", &inputsOnly)) rrFullscreenSource = inputsOnly ? 1u : 0u;
		if (ImGui::Checkbox("Prepared RR outputs only", &outputsOnly)) rrFullscreenSource = outputsOnly ? 2u : 0u;
		int signal = int(rrSignalView);
		if (ImGui::Combo("Full-screen signal", &signal, "Direct diffuse\0Direct specular\0Direct combined\0Indirect diffuse\0Indirect specular\0Indirect combined\0Validity\0")) rrSignalView = uint32_t(signal);
		ImGui::SliderFloat("Prepared RR preview exposure (EV)", &rrPreviewExposure, -8.0f, 8.0f, "%.1f");
		ImGui::TextWrapped("Raw RR signals, before indirect intensity. Magenta: rejected primary surface. Yellow: depth mismatch. Orange: unavailable indirect or current RR output. Dark checker squares: untraced input. Validity shows available primary surfaces green; valid radiance can still be black.");
		if (ImGui::TreeNodeEx("Prepared RR buffers", ImGuiTreeNodeFlags_DefaultOpen)) {
			int selected = int(rrBuffer);
			static float scale = 0.5f;
			if (ImGui::Combo("Buffer", &selected, "Linear depth\0Motion\0Direct diffuse output\0Direct specular output\0Indirect diffuse output\0Indirect specular output\0Direct diffuse input\0Direct specular input\0Indirect diffuse input\0Indirect specular input\0Normal / roughness\0Specular albedo\0Diffuse albedo\0Direct combined\0Indirect combined\0Validity\0")) rrBuffer = uint32_t(selected);
			ImGui::SliderFloat("Display scale", &scale, 0.1f, 1.0f);
			rrViewerRequestFrame = globals::game::graphicsState->GetFrameCount();
			if (rrDebugPreviewValid && rrDebugPreview) {
				ImGui::Text("Physical source: %u x %u | logical RR grid: %u x %u", rrDebugStorageSize[0], rrDebugStorageSize[1], rrDebugPreview->desc.Width, rrDebugPreview->desc.Height);
				ImGui::Text("Preview: %u x %u | generated frame: %u", rrDebugPreview->desc.Width, rrDebugPreview->desc.Height, rrDebugPreviewFrame);
				ImGui::Image(reinterpret_cast<ImTextureID>(rrDebugPreview->srv.get()), { float(rrDebugPreview->desc.Width) * scale, float(rrDebugPreview->desc.Height) * scale });
			} else ImGui::TextUnformatted("No current preview; enable prepared scene lighting and allow a render frame.");
			ImGui::TreePop();
		}
		ImGui::PopID();
	}

}

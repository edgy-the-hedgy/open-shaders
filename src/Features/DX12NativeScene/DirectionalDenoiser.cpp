#include "DirectionalTrace.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/RayRegenerationSettings.h"

namespace DX12Native
{
	uint32_t DirectionalTrace::DenoiseDirect(Frame& a_frame)
	{
		const auto policy = RayRegeneration::GetSettings();
		const bool eligibleDirect = a_frame.useDirectRR && a_frame.directTraceMode != 0u && policy.enabled;
		const bool eligibleIndirect = a_frame.useIndirectRR && a_frame.indirectRays != 0u && policy.enabled;
		if (!eligibleDirect && !eligibleIndirect) return 0u;
		CS_GPU_PASS("RTDirectLighting::PrepareLightingRR");
		try {
			if (!prepareDirectGuides) prepareDirectGuides.attach(static_cast<ID3D11ComputeShader*>(
				Util::CompileShader(L"Data\\Shaders\\RTDirectLighting\\prepareDirectGuides.cs.hlsl", {}, "cs_5_0")));
			if (!packRRSignals) packRRSignals.attach(static_cast<ID3D11ComputeShader*>(
				Util::CompileShader(L"Data\\Shaders\\RTDirectLighting\\packRRSignals.cs.hlsl", {}, "cs_5_0")));
			if (!prepareDirectGuides || !packRRSignals) return 0u;
			// RR contexts always reconstruct at true render size. Only native
			// checkerboard inputs use half the horizontal signal storage.
			const auto size = a_frame.size;
			const bool widthSupportsCheckerboard = size[0] >= 2u && (size[0] & 1u) == 0u;
			const bool directCB = eligibleDirect && a_frame.directTraceMode == 3u && widthSupportsCheckerboard;
			const bool indirectCB = eligibleIndirect && a_frame.indirectTraceMode == 3u && widthSupportsCheckerboard;
			if (directRRSize != size || directRRCheckerboard != directCB ||
				indirectRRCheckerboard != indirectCB || !directRRTextures[0]) {
				std::array<std::unique_ptr<Texture2D>, 13> textures;
				static constexpr std::array names{ "Depth", "Motion", "FilteredDirectDiffuse",
					"FilteredDirectSpecular", "FilteredIndirectDiffuse", "FilteredIndirectSpecular",
					"InputDirectDiffuse", "InputDirectSpecular", "InputIndirectDiffuse",
					"InputIndirectSpecular", "NormalRoughness", "SpecularAlbedo", "DiffuseAlbedo" };
				for (uint32_t i = 0; i < textures.size(); ++i) {
					D3D11_TEXTURE2D_DESC desc{};
					const bool halfWidth = (i == 6u || i == 7u) ? directCB :
						(i == 8u || i == 9u) ? indirectCB : false;
					desc.Width = halfWidth ? size[0] / 2u : size[0];
					desc.Height = size[1];
					desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
					desc.Format = i == 0 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
					desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
					const auto name = std::format("RTDirectLighting::RR{}", names[i]);
					textures[i] = std::make_unique<Texture2D>(desc, name.c_str());
					D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
					srv.Format = desc.Format; srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
					srv.Texture2D.MipLevels = 1;
					D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
					uav.Format = desc.Format; uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
					textures[i]->CreateSRV(srv);
					textures[i]->CreateUAV(uav);
				}
				directRRTextures = std::move(textures);
				directRRSize = size;
				directRRCheckerboard = directCB;
				indirectRRCheckerboard = indirectCB;
				directRRGeneration = UINT64_MAX;
				indirectRRGeneration = UINT64_MAX;
			}
			auto* context = backend.d3d11Context.get();
			const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
			struct Grid { std::array<uint32_t, 2> nativeSize, size; uint32_t checkerboard, phase, padding[2]{}; };
			if (!rrGridSettings) rrGridSettings = std::make_unique<ConstantBuffer>(
				ConstantBufferDesc<Grid>(), "RTDirectLighting::RRGrid CB");
			const uint32_t directPhase = a_frame.directSamplePhase & 1u;
			const uint32_t indirectPhase = a_frame.indirectSamplePhase & 1u;
			const uint32_t checkerboardFlags = (directCB ? 1u : 0u) | (indirectCB ? 2u : 0u);
			// Phase is bit-packed by signal. The guide shader ignores both
			// fields, while signal packing interprets them independently.
			rrGridSettings->Update(Grid{ size, size, checkerboardFlags,
				directPhase | (indirectPhase << 1u) });
			auto* cb = rrGridSettings->CB();
			context->CSSetConstantBuffers(0, 1, &cb);
			ID3D11ShaderResourceView* guideInputs[]{ a_frame.output->srv, a_frame.receivers->srv,
				Util::AsReal(targets[RE::RENDER_TARGET::kMOTION_VECTOR].SRV) };
			if (!guideInputs[2]) return 0u;
			ID3D11UnorderedAccessView* guideOutputs[]{
				directRRTextures[0]->uav.get(), directRRTextures[1]->uav.get() };
			context->CSSetShaderResources(0, 3, guideInputs);
			context->CSSetUnorderedAccessViews(0, 2, guideOutputs, nullptr);
			context->CSSetShader(prepareDirectGuides.get(), nullptr, 0);
			context->Dispatch((size[0] + 7u) / 8u, (size[1] + 7u) / 8u, 1u);
			ID3D11UnorderedAccessView* clearedOutputs[7]{};
			context->CSSetUnorderedAccessViews(0, 7, clearedOutputs, nullptr);
			ID3D11ShaderResourceView* inputs[]{
				a_frame.output->srv, a_frame.directSignals[0]->srv, a_frame.directSignals[1]->srv,
				a_frame.directSignals[2]->srv, a_frame.directSignals[3]->srv, a_frame.directSignals[4]->srv,
				a_frame.directSignals[5]->srv, a_frame.directSignals[6]->srv };
			ID3D11UnorderedAccessView* outputs[]{
				directRRTextures[6]->uav.get(), directRRTextures[7]->uav.get(),
				directRRTextures[8]->uav.get(), directRRTextures[9]->uav.get(),
				directRRTextures[10]->uav.get(), directRRTextures[11]->uav.get(),
				directRRTextures[12]->uav.get() };
			context->CSSetShaderResources(0, 8, inputs);
			context->CSSetUnorderedAccessViews(0, 7, outputs, nullptr);
			context->CSSetShader(packRRSignals.get(), nullptr, 0);
			context->Dispatch((size[0] + 7u) / 8u, (size[1] + 7u) / 8u, 1u);
			ID3D11ShaderResourceView* clearedInputs[8]{};
			context->CSSetShaderResources(0, 8, clearedInputs);
			context->CSSetUnorderedAccessViews(0, 7, clearedOutputs, nullptr);
			directRRPhase = directPhase;
			indirectRRPhase = indirectPhase;
			rrPackFrame = a_frame.number;

			// Each RR context maintains independent validity and reset state.
			// A skipped/failed direct context must never inhibit GI RR.
			const bool directReset = directRRGeneration != a_frame.generation ||
				directRRFrame + 1u != a_frame.number ||
				directRRPolicyRevision != policy.revision ||
				directRRNormalMode != a_frame.normalMode ||
				directRRMaterialPolicy != a_frame.rrMaterialPolicy ||
				std::memcmp(&directRRStrengths, &a_frame.strengths, sizeof(float4)) != 0 ||
				std::memcmp(&directRRFalloff, &a_frame.falloffParameters, sizeof(float4)) != 0 ||
				std::memcmp(&directRRStochastic, &a_frame.stochasticParameters, sizeof(float4)) != 0 ||
				directRRIdentities != a_frame.lightIdentities;
			const bool indirectReset = indirectRRGeneration != a_frame.generation ||
				indirectRRFrame + 1u != a_frame.number ||
				indirectRRPolicyRevision != policy.revision ||
				indirectRRMode != a_frame.indirectTraceMode ||
				indirectRRCachedRays != a_frame.indirectRays ||
				indirectRRNormalMode != a_frame.normalMode ||
				indirectRRMaterialPolicy != a_frame.rrMaterialPolicy ||
				std::memcmp(&indirectRRFalloff, &a_frame.falloffParameters, sizeof(float4)) != 0 ||
				std::memcmp(&indirectRRStochastic, &a_frame.stochasticParameters, sizeof(float4)) != 0 ||
				indirectRRIdentities != a_frame.lightIdentities;

			auto dispatch = [&](RedstoneDenoiser& denoiser, bool direct, bool reset) {
				if (!denoiser.Initialize(backend)) return false;
				const uint32_t width = size[0], height = size[1];
				return denoiser.DispatchRayRegeneration(
					directRRTextures[direct ? 7u : 9u]->resource.get(),
					directRRTextures[0]->resource.get(), directRRTextures[1]->resource.get(),
					directRRTextures[10]->resource.get(), directRRTextures[11]->resource.get(),
					directRRTextures[12]->resource.get(),
					directRRTextures[direct ? 3u : 5u]->resource.get(),
					directRRTextures[direct ? 6u : 8u]->resource.get(),
					directRRTextures[direct ? 2u : 4u]->resource.get(), {},
					a_frame.size, width, height, reset, direct ? directCB : indirectCB,
					direct ? directPhase : indirectPhase, direct);
			};
			uint32_t flags = 0u;
			if (eligibleDirect) {
				++directRRAttempts;
				if (dispatch(directDenoiser, true, directReset)) {
					flags |= 1u;
					++directRRSuccesses;
					directRRGeneration = a_frame.generation;
					directRRFrame = a_frame.number;
					directRRPolicyRevision = policy.revision;
					directRRNormalMode = a_frame.normalMode;
					directRRMaterialPolicy = a_frame.rrMaterialPolicy;
					directRRStrengths = a_frame.strengths;
					directRRFalloff = a_frame.falloffParameters;
					directRRStochastic = a_frame.stochasticParameters;
					directRRIdentities = a_frame.lightIdentities;
				} else {
					directRRGeneration = UINT64_MAX;
					++directRRFailures;
				}
			}
			if (eligibleIndirect) {
				++indirectRRAttempts;
				if (dispatch(indirectDenoiser, false, indirectReset)) {
					flags |= 2u;
					++indirectRRSuccesses;
					indirectRRGeneration = a_frame.generation;
					indirectRRFrame = a_frame.number;
					indirectRRPolicyRevision = policy.revision;
					indirectRRMode = a_frame.indirectTraceMode;
					indirectRRCachedRays = a_frame.indirectRays;
					indirectRRNormalMode = a_frame.normalMode;
					indirectRRMaterialPolicy = a_frame.rrMaterialPolicy;
					indirectRRFalloff = a_frame.falloffParameters;
					indirectRRStochastic = a_frame.stochasticParameters;
					indirectRRIdentities = a_frame.lightIdentities;
				} else {
					indirectRRGeneration = UINT64_MAX;
					++indirectRRFailures;
				}
			}
			if (flags) {
				++directRRDispatches; // legacy combined-submit counter
			}
			return flags;
		} catch (const std::exception& error) {
			logger::warn("[RTLighting] Signal RR unavailable; retaining raw lighting: {}", error.what());
			directRRGeneration = UINT64_MAX;
			indirectRRGeneration = UINT64_MAX;
			return 0u;
		}
	}

	void DirectionalTrace::DrawRayRegenerationSettings(bool a_dxrIndirect)
	{
		ImGui::PushID("PreparedRayRegeneration");
		ImGui::Text("Prepared RR submissions: %llu | last frame: %u | budget fallbacks: %llu", directRRDispatches, directRRFrame, directRRBudgetDeferrals);
		ImGui::Text("Native RR: %u x %u | direct checkerboard: %s | GI checkerboard: %s", directRRSize[0], directRRSize[1], directRRCheckerboard ? "on" : "off", indirectRRCheckerboard ? "on" : "off");
		ImGui::Text("Direct RR: %llu success / %llu attempts | GI RR: %llu success / %llu attempts", directRRSuccesses, directRRAttempts, indirectRRSuccesses, indirectRRAttempts);
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
		ImGui::TextUnformatted("Choose signal views in RT Indirect Lighting > RT Lighting Diagnostics.");
		ImGui::PopID();
	}

}

#include "DirectionalTrace.h"
#include "Globals.h"
#include "GpuPass.h"
#include "Utils/D3D.h"
#include "Utils/RayRegenerationSettings.h"
#include <cmath>

namespace RedstoneShadow
{
	void DirectionalTrace::SetRRDebug(uint32_t a_source, uint32_t a_signal, uint32_t a_buffer, float a_exposure)
	{
		if (!std::isfinite(a_exposure)) throw std::invalid_argument("Preview exposure must be finite");
		rrFullscreenSource = std::min(a_source, 2u);
		rrSignalView = std::min(a_signal, 6u);
		rrBuffer = std::min(a_buffer, 15u);
		rrPreviewExposure = std::clamp(a_exposure, -8.0f, 8.0f);
	}
	void DirectionalTrace::PrepareRRPreview(Frame& a_frame, uint32_t a_rrFlags)
	{
		const bool inlineRequested = rrViewerRequestFrame != UINT32_MAX && a_frame.number - rrViewerRequestFrame <= 1;
		if ((!rrFullscreenSource && !inlineRequested) || rrDebugFailed) return;
		CS_GPU_PASS("RedstoneShadows::RRDebugPreview");
		rrDebugPreviewValid = false;
		try {
			if (!rrDebugShader) rrDebugShader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\RedstoneShadows\\rrPreview.cs.hlsl", {}, "cs_5_0")));
			if (!rrDebugShader) { rrDebugFailed = true; return; }
			const auto policy = RayRegeneration::GetSettings();
			const std::array<uint32_t, 2> size{ std::max(1u, a_frame.size[0] >> policy.resolutionMode), std::max(1u, a_frame.size[1] >> policy.resolutionMode) };
			uint32_t selected = rrBuffer;
			if (rrFullscreenSource) {
				static constexpr std::array inputViews{ 6u, 7u, 13u, 8u, 9u, 14u, 15u };
				static constexpr std::array outputViews{ 2u, 3u, 13u, 4u, 5u, 14u, 15u };
				selected = (rrFullscreenSource == 1 ? inputViews : outputViews)[std::min(rrSignalView, 6u)];
			}
			const bool combined = selected == 13 || selected == 14;
			const bool indirect = selected == 4 || selected == 5 || selected == 8 || selected == 9 || selected == 14;
			const bool inputs = (selected >= 6 && selected <= 9) || (combined && rrFullscreenSource == 1);
			const uint32_t sourceIndex = combined ? (indirect ? (inputs ? 8u : 4u) : (inputs ? 6u : 2u)) : std::min(selected, 12u);
			const uint32_t kind = selected == 15 ? 5u : selected == 0 ? 1u : selected == 1 ? 2u : selected == 10 ? 3u : selected >= 11 && selected <= 12 ? 4u : 0u;
			const bool current = rrPackFrame == a_frame.number && directRRSize == size;
			const bool unavailable = !current || !directRRTextures[sourceIndex] || (indirect && !a_frame.indirectRays) ||
				(kind == 0 && !inputs && !(a_rrFlags & (indirect ? 2u : 1u)));
			if (!rrDebugPreview || rrDebugPreview->desc.Width != size[0] || rrDebugPreview->desc.Height != size[1]) {
				D3D11_TEXTURE2D_DESC desc{};
				desc.Width = size[0]; desc.Height = size[1];
				desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
				desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				rrDebugPreview = std::make_unique<Texture2D>(desc, "RedstoneShadows::RRDebugPreview");
				D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = desc.Format; srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
				D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
				uav.Format = desc.Format; uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				rrDebugPreview->CreateSRV(srv); rrDebugPreview->CreateUAV(uav);
			}
			struct Settings {
				std::array<uint32_t, 2> size, nativeSize;
				uint32_t kind, packed, phase, combined;
				float exposure;
				uint32_t indirect, unavailable, padding = 0;
			};
			static_assert(sizeof(Settings) == 48);
			if (!rrDebugSettings) rrDebugSettings = std::make_unique<ConstantBuffer>(ConstantBufferDesc<Settings>(), "RedstoneShadows::RRDebugSettings CB");
			rrDebugSettings->Update(Settings{ size, a_frame.size, kind, inputs && directRRCheckerboard ? 1u : 0u, directRRPhase, combined ? 1u : 0u, rrPreviewExposure, indirect ? 1u : 0u, unavailable ? 1u : 0u });
			auto* context = backend.d3d11Context.get();
			auto* cb = rrDebugSettings->CB();
			ID3D11ShaderResourceView* reads[]{ directRRTextures[sourceIndex] ? directRRTextures[sourceIndex]->srv.get() : nullptr,
				combined && directRRTextures[sourceIndex + 1] ? directRRTextures[sourceIndex + 1]->srv.get() : nullptr,
				a_frame.output->srv, a_frame.indirectRays ? a_frame.directSignals[5]->srv : nullptr };
			auto* write = rrDebugPreview->uav.get();
			context->CSSetConstantBuffers(0, 1, &cb);
			context->CSSetShaderResources(0, 4, reads);
			context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
			context->CSSetShader(rrDebugShader.get(), nullptr, 0);
			context->Dispatch((size[0] + 7) / 8, (size[1] + 7) / 8, 1);
			ID3D11ShaderResourceView* clear[4]{};
			write = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
			context->CSSetShaderResources(0, 4, clear);
			rrDebugStorageSize = kind == 5 ? a_frame.size : directRRTextures[sourceIndex] ? std::array<uint32_t, 2>{ directRRTextures[sourceIndex]->desc.Width, directRRTextures[sourceIndex]->desc.Height } : std::array<uint32_t, 2>{};
			rrDebugIsInput = rrFullscreenSource ? rrFullscreenSource == 1 : inputs; rrDebugUnavailable = unavailable;
			rrDebugPreviewFrame = a_frame.number; rrDebugPreviewValid = true;
		} catch (const std::exception& error) {
			rrDebugFailed = true;
			logger::warn("[RedstoneShadows] RR debug preview unavailable: {}", error.what());
		}
	}

	void DirectionalTrace::DrawRRDebugOverlay()
	{
		if (!rrFullscreenSource) return;
		const auto* viewport = ImGui::GetMainViewport();
		const ImVec2 end{ viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y };
		auto* draw = ImGui::GetBackgroundDrawList();
		draw->AddRectFilled(viewport->Pos, end, IM_COL32(0, 0, 0, 255));
		const bool current = rrDebugPreviewValid && rrDebugPreview && globals::game::graphicsState->GetFrameCount() - rrDebugPreviewFrame <= 1;
		if (current) draw->AddImage(reinterpret_cast<ImTextureID>(rrDebugPreview->srv.get()), viewport->Pos, end);
		const auto label = current ? std::format("Prepared RR {} | RR grid {}x{} | physical source {}x{} | frame {}{}", rrDebugIsInput ? "inputs" : "outputs", rrDebugPreview->desc.Width, rrDebugPreview->desc.Height, rrDebugStorageSize[0], rrDebugStorageSize[1], rrDebugPreviewFrame, rrDebugUnavailable ? " | unavailable this frame" : "") : std::string("Prepared RR preview unavailable for this frame");
		static constexpr float inset = 24.0f;
		draw->AddText({ viewport->Pos.x + inset, viewport->Pos.y + inset }, IM_COL32_WHITE, label.c_str());
	}
}

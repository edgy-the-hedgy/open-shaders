#include "DebugViewPolicy.h"
#include "GpuPass.h"
#include "RedstoneGI.h"
#include "State.h"

namespace
{
	constexpr float kPreviewExposureMin = -8.0f;
	constexpr float kPreviewExposureMax = 8.0f;
	constexpr float kPreviewLabelInset = 24.0f;
}

void RedstoneGI::SetRROutputOnly(bool a_enabled)
{
	if (a_enabled)
		rrDebugSource.store(RRDebugSource::Outputs, std::memory_order_relaxed);
	else {
		auto expected = RRDebugSource::Outputs;
		rrDebugSource.compare_exchange_strong(expected, RRDebugSource::Off, std::memory_order_relaxed);
	}
}

void RedstoneGI::SetRRInputsOnly(bool a_enabled)
{
	if (a_enabled)
		rrDebugSource.store(RRDebugSource::Inputs, std::memory_order_relaxed);
	else {
		auto expected = RRDebugSource::Inputs;
		rrDebugSource.compare_exchange_strong(expected, RRDebugSource::Off, std::memory_order_relaxed);
	}
}

void RedstoneGI::EnsureRRDebugPreviewTexture(const std::array<uint, 2>& a_size)
{
	if (rrPreview && rrPreview->desc.Width == a_size[0] && rrPreview->desc.Height == a_size[1]) return;
	auto desc = texRRSpecularOutput->desc;
	desc.Width = a_size[0];
	desc.Height = a_size[1];
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = 0;
	auto texture = eastl::make_unique<Texture2D>(desc, "RedstoneGI::RRDebugPreview");
	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = desc.Format;
	srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	texture->CreateSRV(srv);
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = desc.Format;
	uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	texture->CreateUAV(uav);
	rrPreview = std::move(texture);
}

void RedstoneGI::PrepareRRDebugPreview(const std::array<uint, 2>& a_size)
{
	const auto source = GetRRDebugSource();
	const bool inputPreview = source == RRDebugSource::Inputs;
	const auto view = GetRRDebugView();

	auto* specular = inputPreview ? texRRSpecularInput.get() : texRRSpecularOutput.get();
	auto* diffuse = inputPreview ? texRRDiffuseInput.get() : texRRDiffuseOutput.get();
	if (source == RRDebugSource::Off || rrPreviewFailed || !specular || !diffuse || !a_size[0] || !a_size[1])
		return;
	CS_GPU_PASS("RedstoneGI::RRDebugPreview");
	try {
		auto* shader = rrPreviewShader.Get(L"Data\\Shaders\\Upscaling\\redstone\\RedstoneGI\\rrDebugPreview.cs.hlsl", {}, "cs_5_0", "main", "RedstoneGI::RRDebugPreview CS");
		if (!shader) {
			rrPreviewFailed = true;
			return;
		}
		EnsureRRDebugPreviewTexture(a_size);
		struct alignas(16) PreviewSettings
		{
			std::array<uint, 2> size;
			float exposureEV;
			uint32_t view;
			float specularScale;
			float diffuseScale;
			uint32_t checkerboard;
			uint32_t checkerboardOrigin;
			uint32_t signalMask;
			uint32_t rawOutputs;
			uint32_t padding[2]{};
		};
		if (!rrPreviewSettings)
			rrPreviewSettings = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<PreviewSettings>(), "RedstoneGI::RRDebugPreview Settings");
		rrPreviewSettings->Update(PreviewSettings{
			a_size, rrPreviewExposureEV.load(std::memory_order_relaxed), static_cast<uint32_t>(view),
			std::exp2(rgiContributionEV + specularContributionEV), std::exp2(rgiContributionEV + diffuseContributionEV),
			inputPreview && rrCheckerboardThisFrame ? 1u : 0u, rrCheckerboardOrigin, GetSignalMask(), !inputPreview && !rrEnabled ? 1u : 0u, {} });

		auto* context = globals::d3d::context;
		winrt::com_ptr<ID3D11Buffer> previousCB;
		context->CSGetConstantBuffers(0, 1, previousCB.put());
		auto* cb = rrPreviewSettings->CB();
		std::array<ID3D11ShaderResourceView*, 5> inputs{
			specular->srv.get(), diffuse->srv.get(),
			inputPreview ? texDiffuseOcclusionInput->srv.get() : texDiffuseOcclusionOutput->srv.get(),
			inputPreview ? texSpecularOcclusionInput->srv.get() : texSpecularOcclusionOutput->srv.get(), texPackedOcclusion->srv.get() };
		auto* output = rrPreview->uav.get();
		context->CSSetConstantBuffers(0, 1, &cb);
		context->CSSetShaderResources(0, static_cast<uint>(inputs.size()), inputs.data());
		context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
		context->CSSetShader(shader, nullptr, 0);
		context->Dispatch((a_size[0] + 7u) / 8u, (a_size[1] + 7u) / 8u, 1);
		inputs.fill(nullptr);
		output = nullptr;
		cb = previousCB.get();
		context->CSSetShaderResources(0, static_cast<uint>(inputs.size()), inputs.data());
		context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
		context->CSSetConstantBuffers(0, 1, &cb);
		rrPreviewSize = a_size;
		rrPreviewGameFrame = rrOutputFrame;
		rrPreviewSequence = globals::state->frameCount;
		rrPreviewView = view;
		rrPreviewSource = source;
		rrPreviewSignalMask = GetSignalMask();
		rrPreviewBypassed = !rrEnabled;
		rrPreviewValid = true;
	} catch (const std::exception& e) {
		rrPreviewFailed = true;
		logger::error("[Redstone] RR debug preview unavailable: {}", e.what());
	}
}

bool RedstoneGI::IsRRDebugPreviewCurrent() const
{
	const bool geometry = rrPreviewView == RRDebugView::GeometryDepth;
	const bool sourceReady = geometry || (rrPreviewSource == RRDebugSource::Inputs ? rrLightingReady : rrLightingReady && (!rrEnabled || rrProviderSucceeded));
	return rrPreview && rrPreviewSignalMask == GetSignalMask() && rrPreviewBypassed == !rrEnabled && rrPreviewSource == GetRRDebugSource() && rrPreviewView == GetRRDebugView() && globals::game::frameCounter && globals::state &&
	       Redstone::IsDebugPreviewCurrent(rrPreviewValid && sourceReady, rrPreviewGameFrame,
			   static_cast<unsigned>(*globals::game::frameCounter), rrPreviewSequence, globals::state->frameCount);
}

void RedstoneGI::DrawRRDebugSettings()
{
	bool enabled = IsRROutputOnly();
	if (ImGui::Checkbox(T("feature.upscaling.redstone.rr_output_only", "RR Output Only (temporary debug view)"), &enabled))
		SetRROutputOnly(enabled);
	bool inputsEnabled = IsRRInputsOnly();
	if (ImGui::Checkbox(T("feature.upscaling.redstone.rr_inputs_only", "RR Inputs Only (temporary debug view)"), &inputsEnabled))
		SetRRInputsOnly(inputsEnabled);
	if (!IsRRDebugVisible())
		return;
	int view = static_cast<int>(GetRRDebugView());
	const char* views[]{
		T("feature.upscaling.redstone.rr_view_combined", "Diffuse + Specular IL"),
		T("feature.upscaling.redstone.rr_view_diffuse", "Diffuse IL Only"),
		T("feature.upscaling.redstone.rr_view_specular", "Specular IL Only"),
		T("feature.upscaling.redstone.rr_view_diffuse_ao", "Diffuse AO Only"),
		T("feature.upscaling.redstone.rr_view_specular_occlusion", "Specular Occlusion Only"),
		T("feature.upscaling.redstone.rr_view_combined_occlusion", "Combined Occlusion (R/G)"),
		T("feature.upscaling.redstone.rr_view_all", "All Four Signals (2x2)"),
		T("feature.upscaling.redstone.rr_view_geometry", "Geometry Depth (white near / black far / red invalid)")
	};
	if (ImGui::Combo(T("feature.upscaling.redstone.rr_view", "RR Debug View"), &view, views, static_cast<int>(std::size(views))))
		SetRRDebugView(static_cast<RRDebugView>(view));
	if (GetRRDebugView() == RRDebugView::GeometryDepth) {
		ImGui::TextWrapped("Working linear depth at native render resolution. White is fixed at zero depth; black follows the farthest valid surface each frame. Missing or invalid depth is red. Independent of RR, signal toggles and preview exposure.");
		return;
	}
	float exposure = rrPreviewExposureEV.load(std::memory_order_relaxed);
	if ((view < static_cast<int>(RRDebugView::DiffuseOcclusion) || view == static_cast<int>(RRDebugView::AllSignals)) && ImGui::SliderFloat(T("feature.upscaling.redstone.rr_preview_exposure", "Preview exposure (EV)"), &exposure, kPreviewExposureMin, kPreviewExposureMax, "%.1f"))
		rrPreviewExposureEV.store(exposure, std::memory_order_relaxed);
	ImGui::TextWrapped("%s", T("feature.upscaling.redstone.rr_preview_help", "IL views use scene gains and display exposure. Occlusion shows visibility without strength/exposure. Combined occlusion uses diffuse red and specular green; all four uses labeled panels. Disabled signals and untraced checkerboard input pixels are black. With FSR-RR off, outputs show the raw resolve and are labeled bypassed."));
}

void RedstoneGI::DrawRRDebugOverlay()
{
	if (!IsRRDebugVisible()) return;
	const auto* viewport = ImGui::GetMainViewport();
	const ImVec2 end{ viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y };
	auto* draw = ImGui::GetBackgroundDrawList();
	draw->AddRectFilled(viewport->Pos, end, IM_COL32(0, 0, 0, 255));
	const bool current = IsRRDebugPreviewCurrent();
	if (current) draw->AddImage(reinterpret_cast<ImTextureID>(rrPreview->srv.get()), viewport->Pos, end);
	const bool inputs = GetRRDebugSource() == RRDebugSource::Inputs;
	const char* source = inputs ? "Inputs" : rrEnabled ? "RR Outputs" : "Outputs (FSR-RR bypassed)";
	const char* views[]{ "Diffuse + Specular IL", "Diffuse IL", "Specular IL", "Diffuse AO", "Specular Occlusion", "Occlusion: diffuse R / specular G", "All Four Signals", "Geometry Depth" };
	const auto view = static_cast<uint32_t>(GetRRDebugView());
	const auto label = current ? (view == static_cast<uint32_t>(RRDebugView::GeometryDepth) ?
		std::format("Geometry Depth | native grid {}x{} | white: zero / black: frame maximum / red: invalid", rrPreviewSize[0], rrPreviewSize[1]) :
		std::format("{} - {} | RR grid {}x{} (raw)", source, views[std::min(view, 7u)], rrPreviewSize[0], rrPreviewSize[1])) :
		rrPreviewFailed ? std::string("RR preview unavailable - shader/resource failure") : std::string("RR preview unavailable for this frame");
	draw->AddText({ viewport->Pos.x + kPreviewLabelInset, viewport->Pos.y + kPreviewLabelInset }, IM_COL32_WHITE, label.c_str());
	if (current && GetRRDebugView() == RRDebugView::AllSignals) {
		const char* names[]{ "Diffuse IL", "Specular IL", "Diffuse AO", "Specular Occlusion" };
		const auto mask = GetSignalMask();
		for (uint32_t i = 0; i < 4; ++i) {
			const auto panelLabel = std::format("{}{}", names[i], (mask & (1u << i)) ? "" : " (disabled)");
			const ImVec2 position{ viewport->Pos.x + (i & 1u) * viewport->Size.x * 0.5f + kPreviewLabelInset,
				viewport->Pos.y + (i >> 1u) * viewport->Size.y * 0.5f + (i < 2u ? kPreviewLabelInset * 2.0f : kPreviewLabelInset) };
			draw->AddText(position, IM_COL32_WHITE, panelLabel.c_str());
		}
	}
}

#include "MaterialSidecar.h"
#include "GpuPass.h"
#include "Utils/D3D.h"

#include <dxgi1_4.h>
#include <algorithm>
#include <format>
#include <new>
#include <ranges>
#include <stdexcept>

namespace NativePBR
{
	namespace
	{
		constexpr std::array<DXGI_FORMAT, MaterialSidecar::kCount> kFormats{
			DXGI_FORMAT_R32_UINT,
			DXGI_FORMAT_R32G32B32A32_FLOAT,
			DXGI_FORMAT_R32G32B32A32_FLOAT,
			DXGI_FORMAT_R16G16B16A16_FLOAT,
			DXGI_FORMAT_R16G16B16A16_FLOAT,
			DXGI_FORMAT_R16G16B16A16_FLOAT
		};

		uint64_t RequiredBytes(uint32_t a_width, uint32_t a_height)
		{
			uint64_t perPixel = 0;
			for (const auto bytes : kSidecarBytesPerPixel)
				perPixel += bytes;
			return uint64_t(a_width) * a_height * perPixel;
		}

		bool HasVideoBudget(ID3D11Device* a_device, uint64_t a_required)
		{
			winrt::com_ptr<IDXGIDevice> dxgiDevice;
			if (FAILED(a_device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()))))
				return false;
			winrt::com_ptr<IDXGIAdapter> adapter;
			if (FAILED(dxgiDevice->GetAdapter(adapter.put())))
				return false;
			winrt::com_ptr<IDXGIAdapter3> adapter3;
			if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(adapter3.put()))))
				return false;
			DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
			if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)) ||
				memory.Budget <= memory.CurrentUsage)
				return false;
			const auto available = memory.Budget - memory.CurrentUsage;
			// Reserve a quarter of the currently free local budget for Skyrim and other RT features.
			return a_required <= available - available / 4;
		}
	}

	void MaterialSidecar::Prepare(ID3D11Device* a_device)
	{
		if (!a_device || (shader && validation && drawConstants && validationConstants && writeAll))
			return;
		try {
			shader.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(
				L"Data\\Shaders\\NativePBR\\MaterialSidecar.ps.hlsl", {}, "ps_5_0")));
			previewShader.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(
				L"Data\\Shaders\\NativePBR\\PreviewOpaque.ps.hlsl", {}, "ps_5_0")));
			if (previewShader)
				Util::SetResourceName(previewShader.get(), "NativePBR::PreviewOpaque");
			validation.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
				L"Data\\Shaders\\NativePBR\\ValidateSidecar.cs.hlsl", {}, "cs_5_0")));
			if (!shader || !validation)
				throw std::runtime_error("Sidecar capture/validation shader compile failed");
			D3D11_BUFFER_DESC cb{};
			cb.ByteWidth = sizeof(Constants);
			cb.Usage = D3D11_USAGE_DEFAULT;
			cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			winrt::check_hresult(a_device->CreateBuffer(&cb, nullptr, drawConstants.put()));
			winrt::check_hresult(a_device->CreateBuffer(&cb, nullptr, validationConstants.put()));
			D3D11_BLEND_DESC blend{};
			for (auto& rt : blend.RenderTarget) {
				rt.BlendEnable = FALSE;
				rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			}
			winrt::check_hresult(a_device->CreateBlendState(&blend, writeAll.put()));
			status.clear();
		} catch (const std::exception& e) {
			status = e.what();
		}
	}

	void MaterialSidecar::BeginFrame(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_mainTarget,
		uint32_t a_frame, uint32_t a_width, uint32_t a_height)
	{
		CS_GPU_PASS("NativePBR::SidecarBegin");
		ready = recording = false;
		publishedDraws.reset();
		activeMaterial = 0;
		activeRevision = 0;
		activeReference.clear();
		activeShape.clear();
		expectedMain = nullptr;
		draws.clear();
		nextDraw = 1;
		if (!a_device || !a_context || !a_mainTarget || !shader || !validation || !writeAll || !a_width || !a_height) {
			status = "Sidecar shaders, device, or dimensions unavailable";
			++failedFrames;
			return;
		}
		try {
			if (width != a_width || height != a_height || !targets[0].texture) {
				if (!HasVideoBudget(a_device, RequiredBytes(a_width, a_height)))
					throw std::runtime_error("DXGI local budget cannot accommodate full-resolution PBR sidecar");
				std::array<Target, kCount> replacements;
				for (size_t i = 0; i < kCount; ++i) {
					D3D11_TEXTURE2D_DESC desc{};
					desc.Width = a_width;
					desc.Height = a_height;
					desc.MipLevels = desc.ArraySize = 1;
					desc.Format = kFormats[i];
					desc.SampleDesc.Count = 1;
					desc.Usage = D3D11_USAGE_DEFAULT;
					desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
					if (i == static_cast<size_t>(SidecarTarget::Identity))
						desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
					winrt::check_hresult(a_device->CreateTexture2D(&desc, nullptr, replacements[i].texture.put()));
					winrt::check_hresult(a_device->CreateRenderTargetView(replacements[i].texture.get(), nullptr, replacements[i].rtv.put()));
					winrt::check_hresult(a_device->CreateShaderResourceView(replacements[i].texture.get(), nullptr, replacements[i].srv.put()));
					if (i == static_cast<size_t>(SidecarTarget::Identity))
						winrt::check_hresult(a_device->CreateUnorderedAccessView(replacements[i].texture.get(), nullptr, replacements[i].uav.put()));
				}
				targets = std::move(replacements);
				width = a_width;
				height = a_height;
			}
			const float clear[4]{};
			a_context->ClearRenderTargetView(targets[0].rtv.get(), clear);
			expectedMain.copy_from(a_mainTarget);
			frame = a_frame;
			recording = true;
			status.clear();
		} catch (const std::exception& e) {
			status = e.what();
			++failedFrames;
		}
	}

	void MaterialSidecar::BeginPass(uint64_t a_materialIdentity, uint64_t a_materialRevision, std::string a_referenceKey, std::string a_shapeName)
	{
		activeMaterial = recording && a_materialRevision ? a_materialIdentity : 0;
		activeRevision = activeMaterial ? a_materialRevision : 0;
		activeReference = activeMaterial ? std::move(a_referenceKey) : std::string{};
		activeShape = activeMaterial ? std::move(a_shapeName) : std::string{};
	}

	void MaterialSidecar::EndPass()
	{
		activeMaterial = 0;
		activeRevision = 0;
		activeReference.clear();
		activeShape.clear();
	}

	void MaterialSidecar::ReplayDrawIndexed(ID3D11DeviceContext* a_context, UINT a_count, UINT a_first, INT a_base, bool a_previewReplacement)
	{
		if (!recording || !activeMaterial || !a_context || a_count == 0)
			return;
		CS_GPU_PASS("NativePBR::SidecarRaster");
		// This call is made AFTER the original draw has finished, on the same immediate context.
		// It must not perturb any deferred render-target, stencil, or shader state.
		std::array<winrt::com_ptr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> oldTargets;
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> oldRaw{};
		winrt::com_ptr<ID3D11DepthStencilView> dsv;
		ID3D11DepthStencilView* oldDsv = nullptr;
		a_context->OMGetRenderTargets(static_cast<UINT>(oldRaw.size()), oldRaw.data(), &oldDsv);
		for (size_t i = 0; i < oldRaw.size(); ++i)
			oldTargets[i].attach(oldRaw[i]);
		dsv.attach(oldDsv);
		if (!dsv || oldRaw[0] != expectedMain.get() ||
			std::ranges::any_of(oldRaw, [](const auto* rtv) { return rtv == nullptr; }) ||
			nextDraw == 0) {
			++rejected;
			return;
		}
		winrt::com_ptr<ID3D11DepthStencilState> oldDepth;
		UINT stencilRef = 0;
		a_context->OMGetDepthStencilState(oldDepth.put(), &stencilRef);
		if (!oldDepth) {
			++rejected;
			return;
		}
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		oldDepth->GetDesc(&depthDesc);
		if (!depthDesc.DepthEnable) {
			++rejected;
			return;
		}
		ID3D11DepthStencilState* replayDepth = nullptr;
		for (const auto& cached : depthStates)
			if (cached.source.get() == oldDepth.get()) {
				replayDepth = cached.replay.get();
				break;
			}
		if (!replayDepth) {
			depthDesc.DepthFunc = D3D11_COMPARISON_EQUAL;
			depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			depthDesc.StencilWriteMask = 0;
			depthDesc.FrontFace.StencilFailOp = depthDesc.FrontFace.StencilDepthFailOp = depthDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
			depthDesc.BackFace.StencilFailOp = depthDesc.BackFace.StencilDepthFailOp = depthDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
			winrt::com_ptr<ID3D11DepthStencilState> state;
			winrt::com_ptr<ID3D11Device> device;
			a_context->GetDevice(device.put());
			if (!device || FAILED(device->CreateDepthStencilState(&depthDesc, state.put()))) {
				++rejected;
				return;
			}
			replayDepth = state.get();
			winrt::com_ptr<ID3D11DepthStencilState> retainedSource;
			retainedSource.copy_from(oldDepth.get());
			depthStates.push_back({ std::move(retainedSource), std::move(state) });
		}
		winrt::com_ptr<ID3D11BlendState> oldBlend;
		float oldFactor[4]{};
		UINT oldMask = 0;
		a_context->OMGetBlendState(oldBlend.put(), oldFactor, &oldMask);
		winrt::com_ptr<ID3D11PixelShader> oldShader;
		a_context->PSGetShader(oldShader.put(), nullptr, nullptr);
		winrt::com_ptr<ID3D11Buffer> oldConstant;
		a_context->PSGetConstantBuffers(13, 1, oldConstant.put());
		const uint32_t ordinal = nextDraw++;
		const Constants constants{ ordinal, 0, static_cast<uint32_t>(activeMaterial), static_cast<uint32_t>(activeMaterial >> 32) };
		a_context->UpdateSubresource(drawConstants.get(), 0, nullptr, &constants, 0, 0);
		std::array<ID3D11RenderTargetView*, kCount> views{};
		for (size_t i = 0; i < kCount; ++i)
			views[i] = targets[i].rtv.get();
		a_context->OMSetRenderTargets(static_cast<UINT>(views.size()), views.data(), dsv.get());
		a_context->OMSetDepthStencilState(replayDepth, stencilRef);
		a_context->OMSetBlendState(writeAll.get(), nullptr, 0xffffffffu);
		a_context->PSSetShader(shader.get(), nullptr, 0);
		ID3D11Buffer* cb = drawConstants.get();
		a_context->PSSetConstantBuffers(13, 1, &cb);
		// The original vertex shader, input layout, IA streams, clipping and viewport remain bound.
		// A caller-side recursion guard skips the replay in DrawIndexed detours.
		a_context->DrawIndexed(a_count, a_first, a_base);
		if (a_previewReplacement && previewShader) {
			CS_GPU_PASS("NativePBR::PreviewOpaqueReplacement");
			ID3D11RenderTargetView* previewTarget = oldRaw[0];
			a_context->OMSetRenderTargets(1, &previewTarget, dsv.get());
			a_context->PSSetShader(previewShader.get(), nullptr, 0);
			a_context->DrawIndexed(a_count, a_first, a_base);
			++previewDraws;
		}
		cb = oldConstant.get();
		a_context->PSSetConstantBuffers(13, 1, &cb);
		a_context->PSSetShader(oldShader.get(), nullptr, 0);
		a_context->OMSetBlendState(oldBlend.get(), oldFactor, oldMask);
		a_context->OMSetDepthStencilState(oldDepth.get(), stencilRef);
		a_context->OMSetRenderTargets(static_cast<UINT>(oldRaw.size()), oldRaw.data(), dsv.get());
		try {
			draws.push_back({ ordinal, activeMaterial, activeRevision, activeReference, activeShape });
			++replayed;
		} catch (const std::bad_alloc&) {
			recording = false;
			status = "Sidecar draw table allocation exhausted";
			++failedFrames;
		}
	}

	void MaterialSidecar::FinishFrame(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_finalDepth)
	{
		CS_GPU_PASS("NativePBR::SidecarValidateCoverage");
		activeMaterial = 0;
		ready = false;
		if (!recording)
			return;
		recording = false;
		if (!a_context || !a_finalDepth || !validation || draws.empty()) {
			status = draws.empty() ? "No eligible PBR draws" : "Final opaque depth unavailable";
			return;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC depthView{};
		a_finalDepth->GetDesc(&depthView);
		const bool depth24 = depthView.Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		const bool depth32 = depthView.Format == DXGI_FORMAT_R32_FLOAT ||
			depthView.Format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
		if (depthView.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
			(!depth24 && !depth32)) {
			status = std::format("Unsupported final depth view for coverage validation: format={}, dimension={}",
				static_cast<uint32_t>(depthView.Format), static_cast<uint32_t>(depthView.ViewDimension));
			++failedFrames;
			return;
		}
		winrt::com_ptr<ID3D11Resource> source;
		a_finalDepth->GetResource(source.put());
		winrt::com_ptr<ID3D11Texture2D> tex = source.try_as<ID3D11Texture2D>();
		D3D11_TEXTURE2D_DESC desc{};
		if (tex)
			tex->GetDesc(&desc);
		if (!tex || desc.Width != width || desc.Height != height || desc.SampleDesc.Count != 1) {
			status = "Final depth resolution/sample count differs from sidecar";
			++failedFrames;
			return;
		}
		// A separate compute pass removes PBR identity when later opaque/non-PBR draws
		// replaced its depth. It never writes to the engine's deferred MRTs.
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> original{};
		winrt::com_ptr<ID3D11DepthStencilView> originalDsv;
		ID3D11DepthStencilView* rawDsv = nullptr;
		a_context->OMGetRenderTargets(static_cast<UINT>(original.size()), original.data(), &rawDsv);
		originalDsv.attach(rawDsv);
		winrt::com_ptr<ID3D11ComputeShader> oldCS;
		winrt::com_ptr<ID3D11Buffer> oldCB;
		std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> oldSRVs;
		std::array<ID3D11ShaderResourceView*, 2> oldReads{};
		winrt::com_ptr<ID3D11UnorderedAccessView> oldUAV;
		a_context->CSGetShader(oldCS.put(), nullptr, nullptr);
		a_context->CSGetConstantBuffers(0, 1, oldCB.put());
		a_context->CSGetShaderResources(0, 2, oldReads.data());
		for (size_t i = 0; i < 2; ++i)
			oldSRVs[i].attach(oldReads[i]);
		a_context->CSGetUnorderedAccessViews(0, 1, oldUAV.put());
		const std::array<uint32_t, 4> constants{ width, height, depth24 ? 1u : 0u, 0u };
		a_context->UpdateSubresource(validationConstants.get(), 0, nullptr, constants.data(), 0, 0);
		a_context->OMSetRenderTargets(0, nullptr, nullptr);
		ID3D11ShaderResourceView* reads[2]{ a_finalDepth, targets[2].srv.get() };
		a_context->CSSetShaderResources(0, 2, reads);
		ID3D11UnorderedAccessView* write = targets[0].uav.get();
		a_context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
		a_context->CSSetShader(validation.get(), nullptr, 0);
		ID3D11Buffer* cb = validationConstants.get();
		a_context->CSSetConstantBuffers(0, 1, &cb);
		a_context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		write = oldUAV.get();
		a_context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
		for (size_t i = 0; i < oldReads.size(); ++i)
			oldReads[i] = oldSRVs[i].get();
		a_context->CSSetShaderResources(0, 2, oldReads.data());
		cb = oldCB.get();
		a_context->CSSetConstantBuffers(0, 1, &cb);
		a_context->CSSetShader(oldCS.get(), nullptr, 0);
		a_context->OMSetRenderTargets(static_cast<UINT>(original.size()), original.data(), originalDsv.get());
		for (auto* rtv : original)
			if (rtv)
				rtv->Release();
		try {
			publishedDraws = std::make_shared<const std::vector<SidecarDraw>>(std::move(draws));
		} catch (const std::bad_alloc&) {
			status = "Sidecar publication allocation exhausted";
			++failedFrames;
			return;
		}
		ready = true;
		status.clear();
	}

	MaterialSidecar::Sources MaterialSidecar::GetSources(uint32_t a_frame) const
	{
		Sources sources{};
		if (!ready || frame != a_frame)
			return sources;
		for (size_t i = 0; i < kCount; ++i)
			sources[i] = targets[i].texture.get();
		return sources;
	}
}

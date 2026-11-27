#include "ReflectionRadianceCapture.h"

#include "Features/LinearLighting.h"
#include "Features/DX12/DX12Control.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Util.h"
#include "Utils/D3D.h"

void IBLInput::ReflectionRadianceCapture::Setup()
{
	if (Allocated())
		return;
	validFaces = 0;
	capturingRadiance = false;
	{
		const auto& reflections = globals::game::renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];
		D3D11_TEXTURE2D_DESC desc{};
		Util::AsReal(reflections.texture)->GetDesc(&desc);
		desc.MipLevels = 1;
		desc.ArraySize = kCubeFaces;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
		reflectionRadiance = eastl::make_unique<Texture2D>(desc, "IBL::ReflectionRadiance");
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = desc.Format;
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
		srv.TextureCube.MipLevels = 1;
		reflectionRadiance->CreateSRV(srv);
		for (UINT face = 0; face < radianceViews.size(); ++face) {
			D3D11_RENDER_TARGET_VIEW_DESC rtv{};
			rtv.Format = desc.Format;
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			rtv.Texture2DArray.FirstArraySlice = face;
			rtv.Texture2DArray.ArraySize = 1;
			DX::ThrowIfFailed(globals::d3d::device->CreateRenderTargetView(reflectionRadiance->resource.get(), &rtv, radianceViews[face].put()));
			Util::SetResourceName(radianceViews[face].get(), "IBL::ReflectionRadiance Face %u RTV", face);
		}
	}
}

std::function<void()> IBLInput::ReflectionRadianceCapture::Begin(bool enabled)
{
	if (!enabled || !Allocated()) {
		validFaces = 0;
		return nullptr;
	}

	const auto lighting = globals::features::linearLighting.GetCommonBufferData();
	const std::array<uint, 2> colorSpace{ lighting.enableLinearLighting, lighting.enableACEScg };
	if (captureColorSpace != colorSpace) {
		validFaces = 0;
		captureColorSpace = colorSpace;
	}
	clearedFaces = 0;
	captureFailed = false;
	capturingRadiance = true;
	const auto flag = static_cast<uint32_t>(State::ExtraShaderDescriptors::CaptureReflectionRadiance);
	globals::state->permutationData.ExtraShaderDescriptor |= flag;
	return [this, flag]() {
		RestoreCaptureTargets();
		globals::state->permutationData.ExtraShaderDescriptor &= ~flag;
		capturingRadiance = false;
		validFaces = captureFailed ? 0 : validFaces | clearedFaces;
	};
}

std::function<void()> IBLInput::ReflectionRadianceCapture::OnRenderPassBegin()
{
	if (!capturingRadiance)
		return nullptr;
	return [this]() { RestoreCaptureTargets(); };
}

void IBLInput::ReflectionRadianceCapture::OnDraw()
{
	if (!capturingRadiance)
		return;
	auto* context = globals::d3d::context;
	std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
	std::array<winrt::com_ptr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> currentTargets;
	const auto readTargets = [&]() {
		context->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), nullptr);
		for (UINT index = 0; index < targets.size(); ++index) {
			currentTargets[index] = nullptr;
			currentTargets[index].attach(targets[index]);
		}
	};
	readTargets();
	const auto& reflections = globals::game::renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];
	if (globals::features::dx12Control.UsesExclusiveMaterials() && captureTargetBound) {
		bool sameFace = false;
		for (UINT face = 0; face < radianceViews.size(); ++face)
			sameFace |= targets[0] == Util::AsReal(reflections.cubeSideRTV[face]) &&
				targets[kRadianceTarget] == radianceViews[face].get();
		if (!sameFace) {
			RestoreCaptureTargets();
			readTargets();
		}
	}
	for (UINT face = 0; face < radianceViews.size(); ++face) {
		if (targets[0] != Util::AsReal(reflections.cubeSideRTV[face]))
			continue;
		auto* currentShader = globals::state->currentShader;
		if (!currentShader || (currentShader->shaderType.get() != RE::BSShader::Type::Lighting &&
								  currentShader->shaderType.get() != RE::BSShader::Type::Sky &&
								  currentShader->shaderType.get() != RE::BSShader::Type::DistantTree &&
								  currentShader->shaderType.get() != RE::BSShader::Type::Effect)) {
			captureFailed = true;
			return;
		}
		auto* shader = globals::state->customPixelShader;
		winrt::com_ptr<ID3D11PixelShader> boundShader;
		context->PSGetShader(boundShader.put(), nullptr, nullptr);
		if (!shader || Util::AsReal(shader->shader) != boundShader.get()) {
			captureFailed = true;
			return;
		}
		winrt::com_ptr<ID3D11BlendState> currentBlend;
		std::array<float, 4> currentBlendFactor{};
		UINT currentSampleMask = 0;
		context->OMGetBlendState(currentBlend.put(), currentBlendFactor.data(), &currentSampleMask);
		const bool radianceTargetBound = captureTargetBound && targets[kRadianceTarget] == radianceViews[face].get();
		if (radianceTargetBound && currentBlend.get() == boundRadianceBlend)
			return;
		if (currentBlend.get() == boundRadianceBlend)
			currentBlend = savedBlend;
		if (!radianceTargetBound)
			RestoreCaptureTargets();
		if (targets[kRadianceTarget] && !radianceTargetBound) {
			captureFailed = true;
			return;
		}
		savedBlend = currentBlend;
		savedBlendFactor = currentBlendFactor;
		savedSampleMask = currentSampleMask;
		auto& entry = captureBlendStates[savedBlend.get()];
		if (!entry.capture) {
			entry.source = savedBlend;
			D3D11_BLEND_DESC desc{};
			if (savedBlend)
				savedBlend->GetDesc(&desc);
			else
				desc.RenderTarget[0] = { FALSE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD,
					D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL };
			if (!desc.IndependentBlendEnable)
				for (auto& target : desc.RenderTarget)
					target = desc.RenderTarget[0];
			desc.IndependentBlendEnable = TRUE;
			desc.RenderTarget[kRadianceTarget] = desc.RenderTarget[0];
			if (FAILED(globals::d3d::device->CreateBlendState(&desc, entry.capture.put()))) {
				captureFailed = true;
				return;
			}
			Util::SetResourceName(entry.capture.get(), "IBL::ReflectionRadiance Blend");
		}
		const UINT faceBit = 1u << face;
		if (!(clearedFaces & faceBit)) {
			CS_GPU_PASS("IBL::ClearReflectionRadiance");
			context->ClearRenderTargetView(radianceViews[face].get(), globals::game::renderer->GetRendererData().clearColor);
			clearedFaces |= faceBit;
		}
		savedTargets = currentTargets;
		savedTargets[kRadianceTarget] = nullptr;
		savedDepth = nullptr;
		context->OMGetRenderTargets(0, nullptr, savedDepth.put());
		targets[kRadianceTarget] = radianceViews[face].get();
		context->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), savedDepth.get());
		context->OMSetBlendState(entry.capture.get(), savedBlendFactor.data(), savedSampleMask);
		boundRadianceBlend = entry.capture.get();
		captureTargetBound = true;
		return;
	}
}

void IBLInput::ReflectionRadianceCapture::RestoreCaptureTargets()
{
	if (captureTargetBound && globals::features::dx12Control.UsesExclusiveMaterials()) {
		// Restore only the attachment and blend object we installed. Other
		// attachments, depth and blend factors may have changed legitimately.
		auto* context = globals::d3d::context;
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
		std::array<winrt::com_ptr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> currentTargets;
		winrt::com_ptr<ID3D11DepthStencilView> depth;
		context->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), depth.put());
		for (UINT index = 0; index < targets.size(); ++index)
			currentTargets[index].attach(targets[index]);
		const bool captureStillBound = std::any_of(radianceViews.begin(), radianceViews.end(),
			[&](const auto& view) { return view.get() == targets[kRadianceTarget]; });
		if (captureStillBound) {
			targets[kRadianceTarget] = savedTargets[kRadianceTarget].get();
			context->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), depth.get());
		}
		winrt::com_ptr<ID3D11BlendState> currentBlend;
		std::array<float, 4> blendFactor{};
		UINT sampleMask = 0;
		context->OMGetBlendState(currentBlend.put(), blendFactor.data(), &sampleMask);
		if (currentBlend.get() == boundRadianceBlend)
			context->OMSetBlendState(savedBlend.get(), blendFactor.data(), sampleMask);
	} else if (captureTargetBound) {
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
		globals::d3d::context->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), nullptr);
		const bool captureStillBound = std::any_of(radianceViews.begin(), radianceViews.end(),
			[&](const auto& view) { return view.get() == targets[kRadianceTarget]; });
		for (auto* target : targets)
			if (target)
				target->Release();
		if (captureStillBound) {
			for (UINT index = 0; index < targets.size(); ++index)
				targets[index] = savedTargets[index].get();
			globals::d3d::context->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), savedDepth.get());
			globals::d3d::context->OMSetBlendState(savedBlend.get(), savedBlendFactor.data(), savedSampleMask);
		}
	}
	captureTargetBound = false;
	boundRadianceBlend = nullptr;
	for (auto& target : savedTargets)
		target = nullptr;
	savedDepth = nullptr;
	savedBlend = nullptr;
}


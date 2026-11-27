#include "NativeRawInputCapture.h"

#include "Features/DX12/DX12GpuRetirement.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "Utils/D3D.h"
#include "Utils/FileSystem.h"

#include <algorithm>
#include <directx/d3dx12.h>

namespace NativePBR
{
	using namespace DX12Native;
	using namespace MaterialFrameContract;
	namespace
	{
		constexpr auto kFormat = DXGI_FORMAT_R32G32B32A32_UINT;
		constexpr UINT kMRTs = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
		constexpr UINT kUAVs = D3D11_1_UAV_SLOT_COUNT - kMRTs;
		struct DrawIdentity { uint32_t instance, tokenBase, primitives, reserved = 0; };
	}

	bool RawInputFrame::Matches(RasterIdentityStamp requested, uint32_t viewWidth, uint32_t viewHeight) const
	{
		return stamp == requested && width >= viewWidth && height >= viewHeight && draws && planes && planes->resource &&
			readyFence && ValidProducerSignal(readyValue, readyFence->GetCompletedValue()) &&
			evidence && evidence->stamp == stamp && evidence->width == width && evidence->height == height &&
			evidence->texture && evidence->texture->resource && evidence->tokenTable && evidence->tokenCount &&
			evidence->readyFence && ValidProducerSignal(evidence->readyValue, evidence->readyFence->GetCompletedValue());
	}

	RawInputCapture::~RawInputCapture()
	{
		Reset();
		for (auto& slot : slots) {
			if (slot && !GpuFenceRetired(slot->readyFence.get(), slot->readyValue))
				RetireAfterFences(std::move(slot), owner.d3d12Device,
					std::vector<FenceDependency>{FencePoint(producer.fence12.get(), slot->readyValue)});
		}
	}

	bool RawInputCapture::Seal()
	{
		if (!active) return false;
		const auto ready = producer.Next();
		if (FAILED(owner.d3d11Context->Signal(producer.fence11.get(), ready))) {
			failure = "Raw input fence signal failed";
			active.reset(); // Slot retains the untracked UINT64_MAX lease.
			return false;
		}
		active->readyValue = ready;
		owner.d3d11Context->Flush();
		return true;
	}

	void RawInputCapture::Reset()
	{
		published.reset();
		if (active) {
			Seal();
			active.reset();
		}
		failedAllocationFrame = UINT32_MAX;
	}

	bool RawInputCapture::Prepare(const GraphicsBackend& backend, RasterIdentityStamp stamp, uint32_t width, uint32_t height)
	{
		if (active && active->stamp == stamp)
			return active->width == width && active->height == height;
		if (active) Reset();
		published.reset();
		if (failedAllocationFrame == stamp.frame) return false;
		if (!backend.d3d11Device || !backend.d3d11Context || !backend.d3d12Device ||
			!width || !height || backend.d3d11Device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1)
			return false;
		if (!shader && !shaderRequest) {
			shaderRequest = std::make_shared<ShaderRequest>();
			auto request = shaderRequest;
			globals::shaderCache->EnqueueStandaloneShaderCompile(
				(Util::PathHelpers::GetShadersPath() / "NativePBR" / "InputOnly.ps.hlsl").wstring(), "main", {},
				SIE::ShaderCache::StandaloneShaderClass::Pixel, [request](ID3D11DeviceChild* child) {
					winrt::com_ptr<ID3D11DeviceChild> owned;
					owned.attach(child);
					std::scoped_lock lock(request->mutex);
					if (owned) owned->QueryInterface(IID_PPV_ARGS(request->shader.put()));
					request->complete = true;
				});
		}
		if (!shader) {
			std::scoped_lock lock(shaderRequest->mutex);
			shader = shaderRequest->shader;
			if (!shader) {
				failure = shaderRequest->complete ? "Input-only FXC compile failed" : "Input-only FXC compile pending";
				failedAllocationFrame = stamp.frame;
				return false;
			}
		}
		if (!producer.fence12) {
			owner = backend;
			producer.Create(owner.d3d12Device.get(), owner.d3d11Device.get(), "NativePBR::InputOnly");
			D3D11_BLEND_DESC blendDesc{};
			blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			DX::ThrowIfFailed(owner.d3d11Device->CreateBlendState(&blendDesc, blend.put()));
			D3D11_DEPTH_STENCIL_DESC depthDesc{};
			depthDesc.DepthEnable = TRUE;
			depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			depthDesc.DepthFunc = D3D11_COMPARISON_EQUAL;
			depthDesc.FrontFace = depthDesc.BackFace = {D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};
			DX::ThrowIfFailed(owner.d3d11Device->CreateDepthStencilState(&depthDesc, depth.put()));
			D3D11_BUFFER_DESC cb{};
			cb.ByteWidth = sizeof(DrawIdentity);
			cb.Usage = D3D11_USAGE_DEFAULT;
			cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			DX::ThrowIfFailed(owner.d3d11Device->CreateBuffer(&cb, nullptr, identity.put()));
		}
		if (!blend || !depth || !identity || owner.d3d11Device.get() != backend.d3d11Device.get() ||
			owner.d3d12Device.get() != backend.d3d12Device.get() ||
			owner.commandQueue.get() != backend.commandQueue.get() || producer.fence12->GetCompletedValue() == UINT64_MAX)
			return false;
		std::shared_ptr<RawInputFrame>* selected = nullptr;
		for (auto& slot : slots) {
			if (slot && slot->readyValue == UINT64_MAX) {
				const auto rescue = producer.Next();
				if (SUCCEEDED(owner.d3d11Context->Signal(producer.fence11.get(), rescue))) {
					slot->readyValue = rescue;
					owner.d3d11Context->Flush();
				}
			}
			if (!slot || (slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue))) {
				selected = &slot;
				if (slot && slot->width == width && slot->height == height) break;
			}
		}
		if (!selected) { failure = "Raw input frame pool busy"; failedAllocationFrame = stamp.frame; return false; }
		auto& slot = *selected;
		if (!slot || slot->width != width || slot->height != height) {
			const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(kFormat, width, height, InputOnly::kPlaneCount, 1, 1, 0,
				D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
			const auto bytes = owner.d3d12Device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
			uint64_t tracked = 0;
			for (const auto& held : slots) if (held) tracked += held->allocationBytes;
			const auto limit = budget.Query(owner.d3d12Device.get(), tracked);
			if (!limit.localBudget || bytes == UINT64_MAX || bytes > limit.effectiveBytes ||
				tracked > limit.effectiveBytes - bytes) {
				failure = "Raw input capture deferred by DXGI budget"; failedAllocationFrame = stamp.frame; return false;
			}
			auto replacement = std::make_shared<RawInputFrame>();
			replacement->allocationBytes = bytes;
			replacement->width = width; replacement->height = height;
			D3D11_TEXTURE2D_DESC texture{};
			texture.Width = width; texture.Height = height; texture.MipLevels = 1;
			texture.ArraySize = InputOnly::kPlaneCount; texture.Format = kFormat;
			texture.SampleDesc.Count = 1; texture.Usage = D3D11_USAGE_DEFAULT;
			texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			replacement->planes = std::make_unique<WrappedResource>(texture, owner.d3d11Device.get(),
				owner.d3d12Device.get(), "NativePBR::RawRasterInputs");
			for (UINT slice = 0; slice < InputOnly::kPlaneCount; ++slice) {
				D3D11_RENDER_TARGET_VIEW_DESC view{};
				view.Format = kFormat; view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
				view.Texture2DArray.FirstArraySlice = slice; view.Texture2DArray.ArraySize = 1;
				DX::ThrowIfFailed(owner.d3d11Device->CreateRenderTargetView(replacement->planes->resource11, &view,
					replacement->views[slice].put()));
			}
			slot = std::move(replacement);
		}
		slot->stamp = stamp; slot->draws = 0; slot->evidence.reset();
		slot->readyFence = producer.fence12; slot->readyValue = UINT64_MAX;
		active = slot;
		const float zero[4]{};
		for (const auto& view : active->views) owner.d3d11Context->ClearRenderTargetView(view.get(), zero);
		failure.clear();
		return true;
	}

	bool RawInputCapture::Replay(const GraphicsBackend& backend, ID3D11DeviceContext* context, ID3D11Texture2D* mask,
		ID3D11VertexShader* expectedVS, ID3D11PixelShader* expectedPS, RasterIdentityStamp stamp,
		uint32_t instance, uint32_t tokenBase, uint32_t primitiveCount, const std::function<void()>& draw)
	{
		if (!context || !mask || !expectedVS || !expectedPS || !draw || !InputOnly::ValidToken(tokenBase, primitiveCount) ||
			!stamp.scope || !stamp.geometryRevision || !stamp.materialRevision || !globals::shaderCache ||
			backend.d3d11Device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1 || failedAllocationFrame == stamp.frame)
			return false;
		try {
			D3D11_PRIMITIVE_TOPOLOGY topology{};
			context->IAGetPrimitiveTopology(&topology);
			if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) return false;
			winrt::com_ptr<ID3D11VertexShader> vs;
			winrt::com_ptr<ID3D11PixelShader> ps;
			winrt::com_ptr<ID3D11GeometryShader> gs;
			winrt::com_ptr<ID3D11HullShader> hs;
			winrt::com_ptr<ID3D11DomainShader> ds;
			context->VSGetShader(vs.put(), nullptr, nullptr);
			context->PSGetShader(ps.put(), nullptr, nullptr);
			context->GSGetShader(gs.put(), nullptr, nullptr);
			context->HSGetShader(hs.put(), nullptr, nullptr);
			context->DSGetShader(ds.put(), nullptr, nullptr);
			if (vs.get() != expectedVS || ps.get() != expectedPS || gs || hs || ds) return false;
			std::array<ID3D11Buffer*, D3D11_SO_BUFFER_SLOT_COUNT> streams{};
			context->SOGetTargets(UINT(streams.size()), streams.data());
			bool streamOutput = false;
			for (auto* stream : streams) if (stream) { streamOutput = true; stream->Release(); }
			if (streamOutput) return false;
			std::array<ID3D11RenderTargetView*, kMRTs> targets{};
			std::array<winrt::com_ptr<ID3D11RenderTargetView>, kMRTs> targetOwners;
			std::array<ID3D11UnorderedAccessView*, kUAVs> uavs{};
			winrt::com_ptr<ID3D11DepthStencilView> dsv;
			context->OMGetRenderTargetsAndUnorderedAccessViews(kMRTs, targets.data(), dsv.put(), kMRTs, kUAVs, uavs.data());
			for (size_t i = 0; i < targets.size(); ++i) targetOwners[i].attach(targets[i]);
			bool hasUAV = false;
			for (auto* uav : uavs) if (uav) { hasUAV = true; uav->Release(); }
			if (hasUAV || !dsv || !targets.back()) return false;
			winrt::com_ptr<ID3D11Resource> maskResource, depthResource;
			targets.back()->GetResource(maskResource.put());
			dsv->GetResource(depthResource.put());
			if (maskResource.get() != static_cast<ID3D11Resource*>(mask)) return false;
			const auto depthTexture = depthResource.try_as<ID3D11Texture2D>();
			if (!depthTexture) return false;
			D3D11_TEXTURE2D_DESC md{}, dd{};
			mask->GetDesc(&md); depthTexture->GetDesc(&dd);
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			dsv->GetDesc(&dsvDesc);
			if (md.Format != DXGI_FORMAT_R16G16B16A16_UNORM || md.ArraySize != 1 || md.MipLevels != 1 ||
				md.SampleDesc.Count != 1 || dd.SampleDesc.Count != 1 || md.Width != dd.Width || md.Height != dd.Height ||
				dsvDesc.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || dsvDesc.Texture2D.MipSlice != 0)
				return false;
			winrt::com_ptr<ID3D11DepthStencilState> oldDepth;
			winrt::com_ptr<ID3D11BlendState> oldBlend;
			winrt::com_ptr<ID3D11Buffer> oldCB;
			UINT stencilRef = 0, sampleMask = 0;
			float blendFactor[4]{};
			context->OMGetDepthStencilState(oldDepth.put(), &stencilRef);
			context->OMGetBlendState(oldBlend.put(), blendFactor, &sampleMask);
			context->PSGetConstantBuffers(0, 1, oldCB.put());
			if (!oldDepth) return false;
			D3D11_DEPTH_STENCIL_DESC originalDepth{};
			oldDepth->GetDesc(&originalDepth);
			if (!originalDepth.DepthEnable || originalDepth.StencilEnable || sampleMask == 0) return false;
			if (oldBlend) {
				D3D11_BLEND_DESC originalBlend{};
				oldBlend->GetDesc(&originalBlend);
				if (originalBlend.AlphaToCoverageEnable ||
					originalBlend.RenderTarget[0].BlendEnable ||
					originalBlend.RenderTarget[originalBlend.IndependentBlendEnable ? kMRTs-1 : 0].BlendEnable)
					return false;
			}
			if (!Prepare(backend, stamp, md.Width, md.Height)) return false;
			const DrawIdentity constants{instance, tokenBase, primitiveCount};
			context->UpdateSubresource(identity.get(), 0, nullptr, &constants, 0, 0);
			std::array<ID3D11RenderTargetView*, InputOnly::kPlaneCount> rawTargets{};
			for (size_t i = 0; i < rawTargets.size(); ++i) rawTargets[i] = active->views[i].get();
			auto* cb = identity.get();
			// Only the original indexed operation is repeated. No engine setup,
			// material binding, raster lighting or depth/stencil writes are repeated.
			context->OMSetRenderTargets(UINT(rawTargets.size()), rawTargets.data(), dsv.get());
			context->OMSetBlendState(blend.get(), nullptr, sampleMask);
			context->OMSetDepthStencilState(depth.get(), 0);
			context->PSSetShader(shader.get(), nullptr, 0);
			context->PSSetConstantBuffers(0, 1, &cb);
			draw();
			auto* restoreCB = oldCB.get();
			context->PSSetConstantBuffers(0, 1, &restoreCB);
			context->PSSetShader(ps.get(), nullptr, 0);
			context->OMSetDepthStencilState(oldDepth.get(), stencilRef);
			context->OMSetBlendState(oldBlend.get(), blendFactor, sampleMask);
			context->OMSetRenderTargets(kMRTs, targets.data(), dsv.get());
			++active->draws;
			return true;
		} catch (...) {
			failure = Util::DescribeCurrentD3DException();
			failedAllocationFrame = stamp.frame;
			return false;
		}
	}

	void RawInputCapture::Publish(ID3D11Texture2D* mask, RasterIdentityStamp stamp, std::span<const RasterPixelIdentity> tokens)
	{
		published.reset();
		if (!active) return;
		const bool same = active->stamp == stamp && active->draws && mask;
		if (Seal() && same) {
			active->evidence = evidenceCapture.Capture(owner, mask, stamp, tokens);
			if (active->Matches(stamp,active->width,active->height)) published = active;
		}
		active.reset();
	}

	std::shared_ptr<const RawInputFrame> RawInputCapture::Get(RasterIdentityStamp stamp) const
	{
		return published && published->Matches(stamp,published->width,published->height) ? published : nullptr;
	}
}

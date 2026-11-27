#pragma once

#include "Buffer.h"
#include <array>
#include <functional>
#include <unordered_map>

namespace IBLInput
{
	/** @brief Shared raster acquisition only. Each backend owns its own capture instance. */
	class ReflectionRadianceCapture
	{
	public:
		void Setup();
		std::function<void()> Begin(bool enabled);
		void OnDraw();
		std::function<void()> OnRenderPassBegin();
		void Invalidate() { validFaces = 0; }
		bool Allocated() const { return bool(reflectionRadiance); }
		bool Ready() const { return reflectionRadiance && validFaces == kAllCubeFaces; }
		UINT ValidFaces() const { return validFaces; }
		UINT ClearedFaces() const { return clearedFaces; }
		bool Failed() const { return captureFailed; }
		UINT Width() const { return reflectionRadiance ? reflectionRadiance->desc.Width : 0; }
		ID3D11Texture2D* Texture() const { return reflectionRadiance ? reflectionRadiance->resource.get() : nullptr; }
		ID3D11ShaderResourceView* View() const { return Ready() ? reflectionRadiance->srv.get() : nullptr; }

	private:
		static constexpr UINT kRadianceTarget = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT - 1;
		static constexpr UINT kCubeFaces = 6;
		static constexpr UINT kAllCubeFaces = (1u << kCubeFaces) - 1u;
	
		eastl::unique_ptr<Texture2D> reflectionRadiance;
		std::array<winrt::com_ptr<ID3D11RenderTargetView>, kCubeFaces> radianceViews;
		std::array<uint, 2> captureColorSpace{};
		UINT clearedFaces = 0;
		UINT validFaces = 0;
		bool capturingRadiance = false;
		bool captureFailed = false;
		bool captureTargetBound = false;
		ID3D11BlendState* boundRadianceBlend = nullptr;
		std::array<winrt::com_ptr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> savedTargets;
		winrt::com_ptr<ID3D11DepthStencilView> savedDepth;
		winrt::com_ptr<ID3D11BlendState> savedBlend;
		std::array<float, 4> savedBlendFactor{};
		UINT savedSampleMask = 0;
		struct CaptureBlendState
		{
			winrt::com_ptr<ID3D11BlendState> source;
			winrt::com_ptr<ID3D11BlendState> capture;
		};
		std::unordered_map<ID3D11BlendState*, CaptureBlendState> captureBlendStates;
	
		void RestoreCaptureTargets();
	};
}

#pragma once

#include "Features/DX12/DX12NativeComposite.h"
#include <array>
#include <memory>
#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

namespace DX12Native
{
	// The encoder writes these shared textures on DX11. A consumer signals its
	// own DX11 fence after encoding, waits on DX12, restores COMMON and signals
	// back to DX11 before reuse. Publication alone does not establish GPU readiness.
	struct UpscalerGuideSet
	{
		enum Index : size_t { Depth, Motion, Reactive, Transparency, Count };
		winrt::com_ptr<ID3D12Device> device;
		std::array<winrt::com_ptr<ID3D11Resource>, Count> encoderOutputs;
		std::array<winrt::com_ptr<ID3D12Resource>, Count> resources;
		bool complete = false;
	};
	struct NativeUpscalerFrame
	{
		uint32_t frame = 0, width = 0, height = 0, displayWidth = 0, displayHeight = 0;
		std::shared_ptr<const NativeComposite::Frame> color;
		std::shared_ptr<const UpscalerGuideSet> guides;
		std::array<ID3D11Resource*, UpscalerGuideSet::Count> sourceIdentities{};
		float jitterX = 0, jitterY = 0, cameraNear = 0, cameraFar = 0, cameraFov = 0, frameTimeMs = 0, preExposure = 1;
		float generationFrameTimeMs = 0;
		uint32_t lightingQuality = kShadowQualityQuality;
		winrt::com_ptr<ID3D12Resource> generationDepth, generationMotion;
		bool resetHistory = false;
		bool MatchesFrame(uint32_t currentFrame, uint32_t w, uint32_t h) const { return frame == currentFrame && width == w && height == h; }
		bool Matches(uint32_t currentFrame, uint32_t w, uint32_t h, ID3D12Device* device,
			const std::array<ID3D11Resource*, UpscalerGuideSet::Count>& sources) const
		{
			return MatchesFrame(currentFrame, w, h) && guides && guides->complete &&
				guides->device.get() == device && sourceIdentities == sources;
		}
	};
}

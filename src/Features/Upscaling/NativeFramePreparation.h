#pragma once

#include "Features/DX12/DX12NativeComposite.h"
#include "Features/DX12/DX12NativeWorldFrame.h"
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
		std::shared_ptr<const NativeWorldFrame> world;
		std::shared_ptr<const UpscalerGuideSet> guides;
		std::array<ID3D11Resource*, UpscalerGuideSet::Count> sourceIdentities{};
		std::array<winrt::com_ptr<ID3D11Resource>, UpscalerGuideSet::Count> sourceLeases;
		bool encoderProduced = false;
		float jitterX = 0, jitterY = 0, cameraNear = 0, cameraFar = 0, cameraFov = 0, frameTimeMs = 0, preExposure = 1;
		float generationFrameTimeMs = 0;
		uint32_t lightingQuality = kShadowQualityQuality;
		winrt::com_ptr<ID3D12Resource> generationDepth, generationMotion;
		bool resetHistory = false;
		bool MatchesFrame(uint32_t currentFrame, uint32_t w, uint32_t h) const { return frame == currentFrame && width == w && height == h; }
		bool MatchesInputs(uint32_t currentFrame, uint32_t w, uint32_t h,
			const std::array<ID3D11Resource*, UpscalerGuideSet::Count>& sources) const
		{
			if (!encoderProduced || !MatchesFrame(currentFrame,w,h) || sourceIdentities != sources) return false;
			for (size_t i=0;i<sources.size();++i) if (!sources[i] || sourceLeases[i].get()!=sources[i]) return false;
			return true;
		}
		// Exclusive consumers require only essential resource availability, not
		// optional native world/geometry evidence or duplicate DX11 identities.
		bool HasCompositeInputs(uint32_t currentFrame, uint32_t w, uint32_t h, ID3D12Device* device) const
		{
			if (!MatchesFrame(currentFrame,w,h) || !encoderProduced || !color || !color->color ||
				!color->producerFence || !color->producerValue || color->producerValue == UINT64_MAX ||
				color->frame != frame || color->width != w || color->height != h ||
				!guides || !guides->complete || guides->device.get()!=device)
				return false;
			for (const auto& guide : guides->resources) {
				if (!guide) return false;
				const auto desc=guide->GetDesc();
				if (desc.Width < w || desc.Height < h) return false;
			}
			return true;
		}
		bool MatchesColor(const std::shared_ptr<const NativeComposite::Frame>& candidate) const
		{
			return candidate && candidate == color && candidate->valid && candidate->color && candidate->producerFence &&
				candidate->producerValue && candidate->frame==frame && candidate->width==width && candidate->height==height;
		}
		bool HasGenerationInputs() const
		{
			if (!generationDepth || !generationMotion || !width || !height) return false;
			const auto d=generationDepth->GetDesc(), m=generationMotion->GetDesc();
			return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && m.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
				d.Format==DXGI_FORMAT_R32_FLOAT && d.Width>=width && d.Height>=height && m.Width>=width && m.Height>=height &&
				d.DepthOrArraySize==1 && m.DepthOrArraySize==1 && d.SampleDesc.Count==1 && m.SampleDesc.Count==1;
		}
		bool Matches(uint32_t currentFrame, uint32_t w, uint32_t h, ID3D12Device* device,
			const std::array<ID3D11Resource*, UpscalerGuideSet::Count>& sources) const
		{
			return MatchesInputs(currentFrame,w,h,sources) && guides && guides->complete && guides->device.get()==device;
		}
	};
}

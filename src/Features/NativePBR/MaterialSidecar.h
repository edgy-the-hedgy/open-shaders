#pragma once

#include "MaterialSidecarContract.h"
#include <d3d11.h>
#include <winrt/base.h>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace NativePBR
{
	/** FXC capture data only. DX12 owns later material evaluation. */
	class MaterialSidecar
	{
	public:
		static constexpr size_t kCount = static_cast<size_t>(SidecarTarget::Count);
		using Sources = std::array<ID3D11Texture2D*, kCount>;

		void Prepare(ID3D11Device* a_device);
		void BeginFrame(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_mainTarget,
			uint32_t a_frame, uint32_t a_width, uint32_t a_height);
		void BeginPass(uint64_t a_materialIdentity, uint64_t a_materialRevision, std::string a_referenceKey, std::string a_shapeName);
		void EndPass();
		void ReplayDrawIndexed(ID3D11DeviceContext* a_context, UINT a_count, UINT a_first, INT a_base, bool a_previewReplacement);
		void FinishFrame(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_finalDepth);
		Sources GetSources(uint32_t a_frame) const;
		std::shared_ptr<const std::vector<SidecarDraw>> GetDrawTable() const { return publishedDraws; }
		size_t PublishedDrawCount() const { return publishedDraws ? publishedDraws->size() : 0; }
		uint32_t GetFrame() const { return frame; }
		uint64_t ReplayedDraws() const { return replayed; }
		uint64_t RejectedDraws() const { return rejected; }
		uint64_t FailedFrames() const { return failedFrames; }
		uint64_t PreviewDraws() const { return previewDraws; }
		bool PreviewReady() const { return static_cast<bool>(previewShader); }
		const std::string& Status() const { return status; }

	private:
		struct Target
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::com_ptr<ID3D11RenderTargetView> rtv;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			winrt::com_ptr<ID3D11UnorderedAccessView> uav;
		};
		struct Constants
		{
			uint32_t ordinal, reserved, materialLow, materialHigh;
		};
		std::array<Target, kCount> targets{};
		winrt::com_ptr<ID3D11PixelShader> shader, previewShader;
		winrt::com_ptr<ID3D11ComputeShader> validation;
		winrt::com_ptr<ID3D11Buffer> drawConstants, validationConstants;
		winrt::com_ptr<ID3D11BlendState> writeAll;
		winrt::com_ptr<ID3D11RenderTargetView> expectedMain;
		struct DepthState
		{
			winrt::com_ptr<ID3D11DepthStencilState> source, replay;
		};
		std::vector<DepthState> depthStates;
		std::vector<SidecarDraw> draws;
		std::shared_ptr<const std::vector<SidecarDraw>> publishedDraws;
		std::string activeReference, activeShape;
		uint64_t activeMaterial = 0, activeRevision = 0, replayed = 0, rejected = 0, failedFrames = 0, previewDraws = 0;
		uint32_t frame = 0, width = 0, height = 0, nextDraw = 1;
		bool recording = false, ready = false;
		std::string status;
	};
}

#pragma once

#include "Features/DX12/DX12FullNativeRasterEvidence.h"
#include "NativeInputOnlyPolicy.h"
#include <functional>
#include <mutex>

namespace NativePBR
{
	struct RawInputFrame
	{
		DX12Native::MaterialFrameContract::RasterIdentityStamp stamp{};
		std::array<std::unique_ptr<WrappedResource>, InputOnly::kPlaneCount> planes;
		std::array<winrt::com_ptr<ID3D11RenderTargetView>, InputOnly::kPlaneCount> views;
		std::shared_ptr<const DX12Native::FullNativeRasterEvidence> evidence;
		winrt::com_ptr<ID3D12Fence> readyFence;
		uint64_t readyValue = 0, allocationBytes = 0;
		uint32_t width = 0, height = 0, draws = 0;
		bool Matches(DX12Native::MaterialFrameContract::RasterIdentityStamp requested,
			uint32_t viewWidth, uint32_t viewHeight) const;
	};

	// Original indexed draw with native MRTs in place of legacy lighting MRTs.
	// The original depth/stencil and rasterizer state remain authoritative.
	class RawInputCapture
	{
	public:
		~RawInputCapture();
		bool CaptureDraw(const GraphicsBackend& backend, ID3D11DeviceContext* context, ID3D11Texture2D* mask,
			ID3D11VertexShader* expectedVS, ID3D11PixelShader* expectedPS,
			DX12Native::MaterialFrameContract::RasterIdentityStamp stamp,
			const NativePBR::InputOnly::DrawMetadata& metadata, const std::function<void()>& draw);
		void Publish(ID3D11Texture2D* mask, DX12Native::MaterialFrameContract::RasterIdentityStamp stamp);
		std::shared_ptr<const RawInputFrame> Get(DX12Native::MaterialFrameContract::RasterIdentityStamp stamp) const;
		void Reset();
		const std::string& Failure() const { return failure; }

	private:
		struct ShaderRequest
		{
			std::mutex mutex;
			winrt::com_ptr<ID3D11PixelShader> shader;
			bool complete = false;
		};
		ID3D11PixelShader* GetShader(bool alphaTest);
		bool Prepare(const GraphicsBackend& backend, DX12Native::MaterialFrameContract::RasterIdentityStamp stamp,
			uint32_t width, uint32_t height);
		bool Seal();
		std::array<std::shared_ptr<ShaderRequest>, 2> shaderRequests;
		std::array<winrt::com_ptr<ID3D11PixelShader>, 2> shaders;
		winrt::com_ptr<ID3D11Buffer> identity;
		GraphicsBackend owner;
		SharedFence producer;
		DX12Native::VideoMemoryBudget budget;
		DX12Native::FullNativeRasterEvidenceCapture evidenceCapture;
		std::array<std::shared_ptr<RawInputFrame>, 2> slots;
		std::shared_ptr<RawInputFrame> active, published;
		std::string failure;
		uint32_t failedAllocationFrame = UINT32_MAX;
	};
}

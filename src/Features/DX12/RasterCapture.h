#pragma once

#include "Globals.h"
#include "Features/Upscaling/DX12SwapChain.h"
#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace DX12
{
	/** @brief Producer-owned DX11 depth copied into a shareable R32_FLOAT DX12 texture. */
	struct RasterFrame
	{
		std::unique_ptr<WrappedResource> depth;
		globals::FrameBuffer camera{};
		winrt::com_ptr<ID3D12Fence> readyFence;
		uint64_t readyValue = 0;
		uint32_t frame = 0, width = 0, height = 0;
		std::vector<std::pair<winrt::com_ptr<ID3D12Fence>, uint64_t>> consumers;
	};

	/** @brief Open-RT-style DX11->DX12 capture with nonblocking queue-fence handoff. */
	class RasterCapture
	{
	public:
		~RasterCapture();
		void Capture(ID3D11ShaderResourceView* depth, const globals::FrameBuffer& camera, uint32_t frame);
		std::shared_ptr<const RasterFrame> Get(uint32_t frame) const;
		void Reset();
		const std::string& Status() const { return status; }
	private:
		std::array<std::shared_ptr<RasterFrame>, 4> slots;
		std::shared_ptr<RasterFrame> current;
		winrt::com_ptr<ID3D11ComputeShader> depthShader;
		SharedFence producer;
		std::string status;
	};
}

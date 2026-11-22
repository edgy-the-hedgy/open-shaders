#pragma once
#include "AccelerationScene.h"
#include "PreparedLightMatching.h"
#include "RasterCapture.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace DX12
{
	/** @brief Raw, intentionally sparse native RayQuery visibility, independent of FSR. */
	struct RawShadowFrame
	{
		std::shared_ptr<const RasterFrame> raster;
		std::shared_ptr<const AccelerationSceneReady> scene;
		std::shared_ptr<const ResolvedLights> preparedLights;
		winrt::com_ptr<ID3D12Resource> bits, camera, lights;
		winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList4> commands;
		winrt::com_ptr<ID3D12Fence> readyFence;
		uint64_t readyValue = 0;
		uint32_t frame = 0, width = 0, height = 0, batchCount = 0, lightCount = 0;
		uint32_t samplingMode = 0, samplingPhase = 0;
		std::vector<std::pair<winrt::com_ptr<ID3D12Fence>, uint64_t>> consumers;
	};

	/** @brief DX12 Native Scene RayQuery producer. Native Composite is its only image consumer. */
	class PreparedShadowTrace
	{
	public:
		~PreparedShadowTrace();
		void Dispatch(std::shared_ptr<const RasterFrame> raster,
			std::shared_ptr<const AccelerationSceneReady> scene,
			std::shared_ptr<const ResolvedLights> lights, uint32_t sampling, uint32_t phase);
		std::shared_ptr<const RawShadowFrame> Get(uint32_t frame) const;
		void Reset() { current.reset(); }
		const std::string& Status() const { return status; }
	private:
		bool Initialize(ID3D12Device* device);
		std::array<std::shared_ptr<RawShadowFrame>, 4> slots;
		std::shared_ptr<RawShadowFrame> current;
		winrt::com_ptr<ID3D12Device> device;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12Fence> completion;
		uint64_t nextFence = 0;
		std::string status;
	};
}

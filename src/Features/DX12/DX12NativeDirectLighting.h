#pragma once

#include "DX12NativeScreenProbe.h"
#include "DX12Resources.h"
#include "VideoMemoryBudget.h"

#include <array>
#include <memory>
#include <span>
#include <string>

namespace DX12Native
{
	/** @brief Absolute-position, linear-radiance input shared by native direct light consumers. */
	struct DirectPointLight
	{
		float3 position{}, radiance{};
		float radius = 0, sourceRadius = 0, fadeZone = 0;
		bool inverseSquare = false;
		bool spot = false;
		std::array<float, 16> worldToSpot{};  // Row-major, row-vector engine projection.
	};
	/** @brief Optional same-frame directional input. No claim of distant caster coverage. */
	struct DirectDirectionalLight
	{
		float4 direction{}, radiance{};  // w on direction indicates valid input.
	};

	/** @brief Fenced packed diffuse/specular radiance with explicit pixel ownership. */
	struct DirectLightingFrame
	{
		static constexpr auto coverage = MaterialFrameContract::LightingCoverage::SelectedDirectLights;
		static constexpr auto encoding = MaterialFrameContract::SignalEncoding::MaterialModulated;
		static constexpr auto sampling = MaterialFrameContract::SamplingLayout::FullResolution;
		static constexpr auto reconstruction = MaterialFrameContract::ReconstructionState::Raw;
		static constexpr uint32_t available = MaterialFrameContract::DirectDiffuse | MaterialFrameContract::DirectSpecular | MaterialFrameContract::Transmission;
		std::shared_ptr<const NativeScreenProbe::MaterialFrame> material;
		winrt::com_ptr<ID3D12Resource> samples, lights, camera, counters, readback, counterInit;
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList> commands;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12Fence> readyFence;
		uint64_t readyValue = 0, allocationBytes = 0;
		uint32_t frame = 0, width = 0, height = 0, rowPitch = 0;
		bool statsPending = false;
		bool directionalIncluded = false;
	};

	/** @brief Consumes the published material frame without retracing primary rays. */
	class DirectLightingTrace
	{
	public:
		~DirectLightingTrace();
		void Poll(uint32_t frame);
		void Reset() { current.reset(); for (auto& slot : slots) if (slot) slot->statsPending = false; Poll(UINT32_MAX); stats = {}; dispatchedLights = 0; failure.clear(); }
		void Dispatch(const GraphicsBackend& backend, std::shared_ptr<const NativeScreenProbe::MaterialFrame> material,
			std::span<const DirectPointLight> lights, bool inputsAvailable, const DirectDirectionalLight& directional,
			uint32_t viewportWidth, uint32_t viewportHeight, bool foliageScatteringEnabled = false);
		std::shared_ptr<const DirectLightingFrame> Get(uint32_t frame) const
		{
			return current && current->frame == frame && current->readyFence &&
				MaterialFrameContract::ValidProducerSignal(current->readyValue, current->readyFence->GetCompletedValue()) ? current : nullptr;
		}
		const std::string& Failure() const { return failure; }
		uint32_t DispatchedLights() const { return dispatchedLights; }
		struct Stats {
			uint32_t frame = 0;
			std::array<uint32_t, 8> counts{};
		};
		const Stats& GetStats() const { return stats; }

	private:
		void Initialize(const GraphicsBackend& backend);
		GraphicsBackend owner;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12Fence> completion;
		std::array<std::shared_ptr<DirectLightingFrame>, 3> slots;
		std::shared_ptr<DirectLightingFrame> current;
		VideoMemoryBudget videoBudget;
		uint64_t nextFence = 0;
		uint32_t dispatchedLights = 0;
		Stats stats;
		std::string failure;
	};
}

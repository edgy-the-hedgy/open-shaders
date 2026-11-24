#pragma once

#include "DX12NativeShadowTrace.h"
#include "DX12NativeDirectLighting.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DX12Native
{
	/** @brief Owns native light selection, SCM eligibility and independent shadow dispatch. */
	class NativeLighting
	{
	public:
		/** @brief Publishes the ordered prepared light selection for the current scene. */
		void PublishPreparedLights(std::shared_ptr<const std::vector<DX12::PreparedLight>> lights)
		{
			shadowLights.store(std::move(lights), std::memory_order_release);
		}
		/** @brief Publishes the runtime LLF light list without mutating raster data. */
		void PublishRuntimeDirectLights(uint32_t frame, std::vector<DirectPointLight> lights);
		uint32_t DirectLightCount() const { return direct.DispatchedLights(); }
		const DirectLightingTrace::Stats& DirectStats() const { return direct.GetStats(); }
		/** @brief Determines whether the observed SCM light is selected for native shadows. */
		bool OwnsStaticForLight(const RE::NiLight* ni, uint32_t frame, bool offloadFlickeringStatic) const;
		/** @brief Captures live light values without altering raster lighting. */
		void ObserveRuntimeLight(RE::BSShadowLight* light, uint32_t referenceID, bool flickers,
			const RE::NiPoint3& restOrigin, uint32_t frame, bool freezeNativeFlickering);
		/** @brief Consumes the existing scene and raster snapshot for independent shadows. */
		void DispatchShadows(const GraphicsBackend& backend, std::shared_ptr<const RasterFrame> captured,
			std::shared_ptr<const TopLevel> built, uint32_t frame, const ShadowTraceControls& controls);
		/** @brief Dispatches direct lighting using the exact material producer snapshot. */
		void DispatchDirect(const GraphicsBackend& backend, std::shared_ptr<const NativeScreenProbe::MaterialFrame> material,
			const ShadowTraceControls& controls);
		/** @brief Retires previous lighting inputs without blocking. */
		void PollDirect(uint32_t frame) { direct.Poll(frame); }
		/** @brief Borrows the same-frame fenced diffuse and specular output. */
		std::shared_ptr<const DirectLightingFrame> GetDirect(uint32_t frame) const { return direct.Get(frame); }
		const std::string& DirectFailure() const { return direct.Failure(); }
		/** @brief Borrows the same-frame fenced shadow contribution for Native Composite. */
		std::shared_ptr<const ShadowMaskFrame> GetShadowMask(uint32_t frame) const { return shadows.Get(frame); }
		/** @brief Keeps the shadow contribution alive through a consumer fence. */
		bool RetainShadowMask(const std::shared_ptr<const ShadowMaskFrame>& frame, ID3D12Fence* fence, uint64_t value)
		{
			return shadows.RetainThrough(frame, fence, value);
		}
		/** @brief Clears the cached stationary light overrides. */
		void ClearFrozenLights();
		/** @brief Invalidates scene-dependent lighting publications. */
		void Reset();

	private:
		/** @brief Builds one ordered prepared/live selection for both lighting passes. */
		std::vector<DX12::PreparedLight> GetUnifiedLights(uint32_t frame) const;
		ShadowMaskTrace shadows;
		DirectLightingTrace direct;
		std::atomic<std::shared_ptr<const std::vector<DX12::PreparedLight>>> shadowLights;
		struct RuntimeLightSample
		{
			const RE::NiLight* source = nullptr;  // Identity of the SCM light actually observed.
			uint32_t referenceID = 0;             // Corresponding prepared native-scene emitter.
			DX12::PreparedLight value;
			bool flickers = false;
		};
		mutable std::mutex runtimeLightMutex;
		uint32_t runtimeLightFrame = UINT32_MAX;
		std::vector<RuntimeLightSample> runtimeLightSamples;
		std::unordered_map<uint32_t, DX12::PreparedLight> frozenNativeLights;
		uint32_t directRuntimeFrame = UINT32_MAX;
		std::vector<DirectPointLight> directRuntimeLights;
	};
}

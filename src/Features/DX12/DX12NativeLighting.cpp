#include "DX12NativeLighting.h"

#include "Globals.h"
#include "GpuPass.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

namespace DX12Native
{
	bool NativeLighting::OwnsStaticForLight(const RE::NiLight* ni, uint32_t frame, bool offloadFlickeringStatic) const
	{
		const auto lights = shadowLights.load(std::memory_order_acquire);
		if (!lights || !ni)
			return false;
		std::scoped_lock lock(runtimeLightMutex);
		if (runtimeLightFrame != frame)
			return false;
	
		const auto found = std::ranges::find_if(runtimeLightSamples, [ni](const RuntimeLightSample& sample) {
			return sample.source == ni;
		});
		if (found == runtimeLightSamples.end() ||
			(found->flickers && !offloadFlickeringStatic))
			return false;
	
		// The shader admits only the published prepared lights. Never suppress a
		// raster static caster for a light not actually selected for DX12 tracing.
		const size_t limit = std::min(lights->size(), size_t{ DX12Native::kMaxShadowLights });
		for (size_t i = 0; i < limit; ++i) {
			const auto& candidate = (*lights)[i];
			if (candidate.referenceID != 0 && candidate.referenceID == found->referenceID)
				return true;
		}
		return false;
	}

	void NativeLighting::ObserveRuntimeLight(RE::BSShadowLight* light, uint32_t referenceID,
		bool flickers, const RE::NiPoint3& restOrigin, uint32_t frame, bool freezeNativeFlickering)
	{
		if (!light)
			return;
		auto* ni = light->light.get();
		const auto lights = shadowLights.load(std::memory_order_acquire);
		if (!ni || !lights)
			return;
		const auto& rt = ni->GetLightRuntimeData();
		const auto& position = ni->world.translate;
		if (!std::isfinite(rt.radius.x) || rt.radius.x <= 0.0f ||
			!std::isfinite(rt.fade) || !std::isfinite(rt.diffuse.red) ||
			!std::isfinite(rt.diffuse.green) || !std::isfinite(rt.diffuse.blue) ||
			!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
			return;
	
		const DX12::PreparedLight* matched = nullptr;
		if (referenceID != 0) {
			const auto it = std::ranges::find_if(*lights, [referenceID](const auto& candidate) {
				return candidate.referenceID == referenceID;
			});
			if (it == lights->end())
				return;  // Never let a different nearby light inherit this source's animation.
			matched = &*it;
		} else {
			float bestDistanceSquared = std::numeric_limits<float>::infinity();
			for (const auto& candidate : *lights) {
				const float dx = candidate.position.x - restOrigin.x;
				const float dy = candidate.position.y - restOrigin.y;
				const float dz = candidate.position.z - restOrigin.z;
				const float errorSquared = dx * dx + dy * dy + dz * dz;
				const float tolerance = std::clamp(candidate.radius * 0.01f, 4.0f, 32.0f);
				if (std::isfinite(errorSquared) && errorSquared < bestDistanceSquared &&
					errorSquared <= tolerance * tolerance) {
					bestDistanceSquared = errorSquared;
					matched = &candidate;
				}
			}
		}
		if (!matched)
			return;
		DX12::PreparedLight live = *matched;
		live.position = float3{ position.x, position.y, position.z };
		live.radius = rt.radius.x;
		live.color = float3{ rt.diffuse.red, rt.diffuse.green, rt.diffuse.blue };
		live.fade = rt.fade;
		std::scoped_lock lock(runtimeLightMutex);
		if (runtimeLightFrame != frame) {
			runtimeLightFrame = frame;
			runtimeLightSamples.clear();
		}
		if (freezeNativeFlickering && flickers && matched->referenceID != 0) {
			// Keep DX11's flicker intact. Only the native shadow has a stable light.
			live.position = float3{ restOrigin.x, restOrigin.y, restOrigin.z };
			live = frozenNativeLights.try_emplace(matched->referenceID, live).first->second;
		}
		// A prepared emitter must not accidentally account for two different SCM
		// lights that happen to occupy the same position.
		const auto it = std::ranges::find_if(runtimeLightSamples, [ni](const auto& sample) {
			return sample.source == ni;
		});
		if (it != runtimeLightSamples.end()) {
			it->referenceID = matched->referenceID;
			it->value = live;
			it->flickers = flickers;
		} else if (runtimeLightSamples.size() < DX12Native::kMaxShadowLights &&
				   !std::ranges::any_of(runtimeLightSamples, [&](const auto& sample) {
					   return sample.referenceID == matched->referenceID;
				   })) {
			runtimeLightSamples.push_back({ ni, matched->referenceID, live, flickers });
		}
	}

	std::vector<DX12::PreparedLight> NativeLighting::GetUnifiedLights(uint32_t frame) const
	{
		const auto lights = shadowLights.load(std::memory_order_acquire);
		std::vector<DX12::PreparedLight> liveLights = lights ? *lights : std::vector<DX12::PreparedLight>{};
		if (!liveLights.empty()) {
			std::scoped_lock lock(runtimeLightMutex);
			if (runtimeLightFrame == frame) {
				for (const auto& runtime : runtimeLightSamples) {
					auto it = std::ranges::find_if(liveLights, [&](const auto& candidate) {
						return candidate.referenceID == runtime.referenceID;
					});
					if (it != liveLights.end())
						*it = runtime.value;
				}
			}
		}
		return liveLights;
	}

	void NativeLighting::DispatchShadows(const GraphicsBackend& backend, std::shared_ptr<const RasterFrame> captured,
		std::shared_ptr<const TopLevel> built, uint32_t frame, const ShadowTraceControls& controls)
	{
		CS_GPU_PASS("NativeLighting::IndependentShadows");
		const auto lights = GetUnifiedLights(frame);
		shadows.Dispatch(backend, std::move(captured), std::move(built), lights, frame, controls);
	}

	void NativeLighting::PublishRuntimeDirectLights(uint32_t frame, std::vector<DirectPointLight> lights)
	{
		std::scoped_lock lock(runtimeLightMutex);
		directRuntimeFrame = frame;
		directRuntimeLights = std::move(lights);
	}

	void NativeLighting::DispatchDirect(const GraphicsBackend& backend, std::shared_ptr<const NativeScreenProbe::MaterialFrame> material,
		const ShadowTraceControls& controls)
	{
		if (!material)
			return;
		const auto prepared = GetUnifiedLights(material->frame);
		const auto toLinear = [](float value) {
			value = std::max(value, 0.0f);
			return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
		};
		std::vector<DirectPointLight> lights;
		lights.reserve(kMaxShadowLights);
		for (const auto& source : prepared) {
			if (lights.size() >= kMaxShadowLights) break;
			lights.push_back({ source.position,
				{ toLinear(source.color.x) * source.fade, toLinear(source.color.y) * source.fade, toLinear(source.color.z) * source.fade },
				source.radius, source.emitterRadius, 0.0f, false });
		}
		const auto preparedCount = lights.size();
		bool runtimeAvailable = false;
		{
			std::scoped_lock lock(runtimeLightMutex);
			runtimeAvailable = directRuntimeFrame == material->frame;
			if (runtimeAvailable) {
				std::vector<bool> matched(preparedCount, false);
				for (const auto& live : directRuntimeLights) {
					size_t best = preparedCount;
					float bestDistance = std::numeric_limits<float>::infinity();
					for (size_t i = 0; i < preparedCount; ++i) {
						if (matched[i]) continue;
						const auto& candidate = lights[i];
						const float dx = live.position.x - candidate.position.x;
						const float dy = live.position.y - candidate.position.y;
						const float dz = live.position.z - candidate.position.z;
						const float d2 = dx*dx + dy*dy + dz*dz;
						const float tolerance = std::clamp(candidate.radius * 0.01f, 4.0f, 32.0f);
						if (d2 <= tolerance*tolerance && d2 < bestDistance) { best = i; bestDistance = d2; }
					}
					if (best != preparedCount) {
						lights[best] = live;
						matched[best] = true;
					} else if (lights.size() < kMaxShadowLights) {
						lights.push_back(live);
					}
				}
			}
		}
		const bool inputsAvailable = runtimeAvailable || !prepared.empty();
		direct.Dispatch(backend, std::move(material), lights, inputsAvailable, controls);
	}

	void NativeLighting::ClearFrozenLights()
	{
		std::scoped_lock lock(runtimeLightMutex);
		frozenNativeLights.clear();
	}

	void NativeLighting::Reset()
	{
		PublishPreparedLights(nullptr);
		{
			std::scoped_lock lock(runtimeLightMutex);
			runtimeLightFrame = UINT32_MAX;
			runtimeLightSamples.clear();
			frozenNativeLights.clear();
			directRuntimeFrame = UINT32_MAX;
			directRuntimeLights.clear();
		}
		shadows.Reset();
		direct.Reset();
	}
}

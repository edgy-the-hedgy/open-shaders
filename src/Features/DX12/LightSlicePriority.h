#pragma once

#include "../../Utils/MathUtils.h"
#include "FullShadowLightConsolidation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <span>
#include <unordered_set>
#include <vector>

namespace DX12Native
{
	/** @brief Slice claim order; lower tiers are served first. */
	enum class SliceTier : uint8_t
	{
		ShadowSlot,  // SCM omits static casters only for a light that got a slice.
		InView,
		OutOfView
	};

	// A light holding a slice keeps it until a rival is clearly stronger, so flicker cannot swap the pair every frame.
	inline constexpr float kHeldSliceBonus = 1.25f;

	/** @brief Identity that survives reordering: the live emitter, else the prepared reference and its position. */
	template <class Light>
	uint64_t SliceKey(const Light& light)
	{
		if (light.runtimeIdentity)
			return light.runtimeIdentity;
		uint64_t key = light.referenceID;
		for (const float axis : { light.position.x, light.position.y, light.position.z })
			key = Util::HashCombineFloat(key, axis);
		return key;
	}

	/** @brief Brightness weighted by how large the light's sphere is from the viewer. */
	template <class Light, class Point>
	float SliceScore(const Light& light, const Point& viewer)
	{
		const double reach = double(light.radius) * light.radius;
		const double distance = FullShadowDetail::DistanceSquared(light.position, viewer);
		const float score = float(NativeLightStrength(light) * (reach / (reach + distance)));
		return std::isfinite(score) && score > 0.0f ? score : 0.0f;
	}

	/** @brief Moves the lights that should own the first `slices` indices to the front, keeping the rest in order. */
	template <class Light>
	class LightSliceOrder
	{
	public:
		/** @param tier SliceTier of a light. @return the reordered lights, valid until the next call. */
		template <class Point, class Tier>
		std::span<const Light> Order(std::span<const Light> lights, size_t slices, const Point& viewer, Tier&& tier)
		{
			const size_t count = std::min(slices, lights.size());
			ranks.clear();
			for (const auto& light : lights)
				ranks.push_back({ tier(light), SliceScore(light, viewer) * (held.contains(SliceKey(light)) ? kHeldSliceBonus : 1.0f) });
			order.resize(lights.size());
			std::iota(order.begin(), order.end(), size_t{ 0 });
			std::ranges::partial_sort(order, order.begin() + count, [this](size_t a, size_t b) {
				if (ranks[a].tier != ranks[b].tier)
					return ranks[a].tier < ranks[b].tier;
				return ranks[a].score != ranks[b].score ? ranks[a].score > ranks[b].score : a < b;
			});
			std::sort(order.begin() + count, order.end());
			held.clear();
			ordered.clear();
			for (size_t i = 0; i < order.size(); ++i) {
				ordered.push_back(lights[order[i]]);
				if (i < count)
					held.insert(SliceKey(ordered.back()));
			}
			return ordered;
		}

	private:
		struct Rank
		{
			SliceTier tier;
			float score;
		};
		std::vector<Rank> ranks;
		std::vector<size_t> order;
		std::vector<Light> ordered;
		std::unordered_set<uint64_t> held;
	};
}

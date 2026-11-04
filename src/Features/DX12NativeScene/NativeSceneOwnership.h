#pragma once
#include <cstdint>
#include <limits>

namespace DX12Native
{
	/** @brief Admits history only from the previous frame with identical scene and shadow policy. */
	template <class Policy>
	constexpr bool NativeShadowHistoryCompatible(uint32_t frame, uint32_t previous, uint64_t scene, uint64_t previousScene,
		const Policy& policy, const Policy& previousPolicy)
	{
		return frame > previous && frame - previous == 1u && scene == previousScene && policy == previousPolicy;
	}
	/** @brief Requires completion of both queues and all borrowed consumers before slot reuse. */
	constexpr bool NativeSceneReusable(bool borrowed, uint64_t complete, uint64_t required,
		uint64_t consumerComplete, uint64_t consumerRequired)
	{
		constexpr auto deviceRemoved = std::numeric_limits<uint64_t>::max();
		return !borrowed && complete != deviceRemoved && consumerComplete != deviceRemoved &&
		       complete >= required && consumerComplete >= consumerRequired;
	}
	/** @brief Performs overflow-safe allocation admission against the owning scene budget. */
	constexpr bool NativeSceneBudgetAllows(uint64_t resident, uint64_t required, uint64_t budget)
	{
		return resident <= budget && required <= budget - resident;
	}
	/** @brief Accumulates bounded allocation estimates without multiplication or addition overflow. */
	constexpr bool NativeSceneBudgetAdd(uint64_t& required, uint64_t bytes, uint32_t count, uint64_t budget)
	{
		if (!count || bytes > budget / count || !NativeSceneBudgetAllows(required, bytes * count, budget))
			return false;
		required += bytes * count;
		return true;
	}
}

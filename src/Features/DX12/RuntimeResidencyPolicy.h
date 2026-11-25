#pragma once

#include <cstddef>
#include <cstdint>

namespace DX12
{
	inline constexpr size_t kMaximumRuntimeShapes = 2048;
	inline constexpr uint64_t kMaximumRuntimeBytes = 256ull * 1024 * 1024;
	inline constexpr uint64_t kMaximumSingleRuntimeShapeBytes = 1024ull * 1024;

	/** @brief Bounds transient CPU geometry without arithmetic underflow. */
	constexpr bool RuntimeShapeFits(size_t residentShapes, uint64_t residentBytes, uint64_t shapeBytes)
	{
		return residentShapes < kMaximumRuntimeShapes &&
		       residentBytes <= kMaximumRuntimeBytes &&
		       shapeBytes > 0 && shapeBytes <= kMaximumSingleRuntimeShapeBytes &&
		       shapeBytes <= kMaximumRuntimeBytes - residentBytes;
	}

	/** @brief Scope and grid epoch must agree before borrowing a diagnostic scene. */
	constexpr bool RuntimeSceneMatches(uint32_t sceneScope, uint64_t sceneEpoch,
		uint32_t liveScope, uint64_t liveEpoch)
	{
		return liveScope != 0 && sceneScope == liveScope && sceneEpoch == liveEpoch;
	}
}

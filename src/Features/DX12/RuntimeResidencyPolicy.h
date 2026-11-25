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

	/** @brief Reject invalid allocation sizes and allocations above current DXGI headroom. */
	constexpr bool RuntimeAllocationFits(uint64_t bytes, uint64_t available)
	{
		return bytes != 0 && bytes != UINT64_MAX && bytes <= available;
	}

	inline constexpr uint32_t kRuntimeObservationLifetime = 600;
	inline constexpr uint32_t kRuntimeSweepInterval = 60;

	/** @brief Unsigned frame subtraction is valid across frame counter rollover. */
	constexpr bool RuntimeObservationExpired(uint32_t frame, uint32_t observed)
	{
		return frame - observed > kRuntimeObservationLifetime;
	}

	/** @brief Scope and grid epoch must agree before borrowing a diagnostic scene. */
	constexpr bool RuntimeSceneMatches(uint32_t sceneScope, uint64_t sceneEpoch,
		uint32_t liveScope, uint64_t liveEpoch)
	{
		return liveScope != 0 && sceneScope == liveScope && sceneEpoch == liveEpoch;
	}
}

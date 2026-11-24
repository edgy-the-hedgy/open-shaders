#pragma once

#include <cstdint>

namespace DX12Native::MaterialFrameContract
{
	inline constexpr uint32_t kMaterialAbiVersion = 1;
	inline constexpr uint32_t kMaterialStride = 32;
	inline constexpr uint32_t kLightingStride = 16;
	inline constexpr uint32_t kBaseOnlyBinding = 1u << 11;

	/** @brief A submitted fence may be incomplete, but must have a valid signal value. */
	constexpr bool ValidProducerSignal(uint64_t value, uint64_t completed)
	{
		return value != 0 && value != UINT64_MAX && completed != UINT64_MAX;
	}

	/** @brief Checks the packed surface ABI before a consumer binds it. */
	constexpr bool ValidMaterialABI(uint32_t version, uint32_t stride)
	{
		return version == kMaterialAbiVersion && stride == kMaterialStride;
	}

	/** @brief Rejects invalid dimensions, unaligned pitches and ByteAddressBuffer offset overflow. */
	constexpr bool ValidLayout(uint32_t width, uint32_t height, uint32_t pitch, uint32_t stride)
	{
		return width && height && stride && pitch % stride == 0 &&
			uint64_t(pitch) >= uint64_t(width) * stride &&
			uint64_t(pitch) * height <= UINT32_MAX;
	}
}

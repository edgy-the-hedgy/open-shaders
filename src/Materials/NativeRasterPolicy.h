#pragma once

#include <cstdint>

namespace NativeRasterPolicy
{
	inline constexpr uint32_t kRouteBit = 1u << 3; // Existing engine pass extension slot.
	inline constexpr uint32_t kInputBit = 1u << 6; // Separate FXC cache identity.
	inline constexpr uint32_t kTechniqueMask = 0x3fu << 24;

	/** @brief Routes only eligible default lighting techniques. */
	constexpr uint32_t RoutePass(uint32_t descriptor, bool eligible, uint32_t incompatible)
	{
		descriptor &= ~kRouteBit;
		if (eligible && !(descriptor & (kTechniqueMask | incompatible))) descriptor |= kRouteBit;
		return descriptor;
	}

	/** @brief Remove both native routing extensions from a raw engine descriptor. */
	constexpr uint32_t StripRoute(uint32_t descriptor)
	{
		return descriptor & ~(kRouteBit | kInputBit);
	}

	/** @brief Converts the raw marker into a native-only pixel permutation. */
	constexpr uint32_t NormalizePixel(uint32_t descriptor, bool enabled, bool deferred)
	{
		const bool routed = (descriptor & kRouteBit) != 0;
		descriptor = StripRoute(descriptor);
		if (routed && enabled && deferred) descriptor |= kInputBit;
		return descriptor;
	}
}

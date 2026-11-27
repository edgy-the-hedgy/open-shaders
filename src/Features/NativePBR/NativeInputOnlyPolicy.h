#pragma once

#include <cstdint>

namespace NativePBR::InputOnly
{
	inline constexpr uint32_t kPlaneCount = 3;
	inline constexpr uint32_t kTokenMask = 0xffffu;
	inline constexpr uint32_t kFrontFace = 1u << 16;
	inline constexpr uint32_t kValid = 1u << 17;

	constexpr bool WholeTriangleList(uint32_t count, uint32_t primitives)
	{
		return primitives && primitives <= kTokenMask && count == primitives * 3u;
	}
	constexpr bool ValidToken(uint32_t base, uint32_t count)
	{
		return base && count && base <= kTokenMask && count - 1u <= kTokenMask - base;
	}
}

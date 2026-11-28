#pragma once

#include <cstdint>

namespace NativePBR::InputOnly
{
	inline constexpr uint32_t kPlaneCount = 4;
	inline constexpr uint32_t kTokenMask = 0xffffu;
	inline constexpr uint32_t kFrontFace = 1u << 16;
	inline constexpr uint32_t kValid = 1u << 17;
	inline constexpr uint32_t kCovered = 1u << 18;
	inline constexpr uint32_t kAlphaShift = 19;
	inline constexpr uint32_t kAlphaMax = 8191;
	inline constexpr uint32_t kAlphaTestFlag = 1u << 20;
	inline constexpr uint32_t kAdditionalAlphaMaskFlag = 1u << 23;

	// Frame-stamped instance and material table indices, plus the live shader
	// descriptors. Do not infer technique from the presence of a texture.
	struct DrawMetadata
	{
		uint32_t instance = UINT32_MAX, tokenBase = 0, primitives = 0, materialIndex = UINT32_MAX;
		uint32_t vertexDescriptor = 0, pixelDescriptor = 0, extraDescriptor = 0, extraFeatureDescriptor = 0;
		constexpr bool AlphaTest() const { return (pixelDescriptor & kAlphaTestFlag) != 0; }
	};
	static_assert(sizeof(DrawMetadata) == 32);

	constexpr uint32_t PackCoverage(uint32_t token, bool front, uint32_t alpha)
	{
		return (token & kTokenMask) | (front ? kFrontFace : 0u) | kValid | kCovered |
			((alpha & kAlphaMax) << kAlphaShift);
	}

	constexpr bool WholeTriangleList(uint32_t count, uint32_t primitives)
	{
		return primitives && primitives <= kTokenMask && count == primitives * 3u;
	}
	constexpr bool ValidToken(uint32_t base, uint32_t count)
	{
		return base && count && base <= kTokenMask && count - 1u <= kTokenMask - base;
	}
}

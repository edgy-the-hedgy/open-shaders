#pragma once

#include <cstdint>

namespace NativePBR::InputOnly
{
	inline constexpr uint32_t kAbiVersion = 6;
	inline constexpr uint32_t kPlaneCount = 4;
	inline constexpr uint32_t kFrontFace = 1u;
	inline constexpr uint32_t kValid = 1u << 1;
	inline constexpr uint32_t kCovered = 1u << 2;
	inline constexpr uint32_t kAlphaShift = 3;
	inline constexpr uint32_t kAlphaMax = 65535;
	inline constexpr uint32_t kMaxPrimitives = UINT32_MAX / 3u;
	inline constexpr uint32_t kAlphaTestFlag = 1u << 20;
	inline constexpr uint32_t kAdditionalAlphaMaskFlag = 1u << 23;

	// Frame-stamped instance and material table indices, plus the live shader
	// descriptors. Do not infer technique from the presence of a texture.
	struct DrawMetadata
	{
		uint32_t instance = UINT32_MAX, geometryIdentity = 0, primitives = 0, materialIndex = UINT32_MAX;
		uint32_t vertexDescriptor = 0, pixelDescriptor = 0, extraDescriptor = 0, extraFeatureDescriptor = 0;
		constexpr bool AlphaTest() const { return (pixelDescriptor & kAlphaTestFlag) != 0; }
	};
	static_assert(sizeof(DrawMetadata) == 32);

	constexpr uint32_t PackCoverage(bool front, uint32_t alpha)
	{
		return (front ? kFrontFace : 0u) | kValid | kCovered | ((alpha & kAlphaMax) << kAlphaShift);
	}

	constexpr bool WholeTriangleList(uint32_t count, uint32_t primitives)
	{
		return primitives && primitives <= kMaxPrimitives && count == primitives * 3u;
	}
	constexpr bool ValidIdentity(uint32_t instance, uint32_t count)
	{
		return instance != UINT32_MAX && count && count <= kMaxPrimitives;
	}
}

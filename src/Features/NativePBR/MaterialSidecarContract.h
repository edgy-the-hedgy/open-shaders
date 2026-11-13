#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <span>
#include <string>

namespace NativePBR
{
	/** Distinct from the eight deferred MRTs; none of their channels are repurposed. */
	enum class SidecarTarget : size_t
	{
		Identity,
		UVAndDdx,
		DdyDepthFacing,
		Basis0,
		Basis1,
		Basis2,
		Count
	};

	inline constexpr std::array<uint32_t, static_cast<size_t>(SidecarTarget::Count)> kSidecarBytesPerPixel{ 4, 16, 16, 8, 8, 8 };

	/** Stored alongside the frame's fenced GPU textures; no borrowed engine pointers. */
	struct SidecarDraw
	{
		uint32_t ordinal = 0;
		uint64_t materialIdentity = 0;
		uint64_t materialRevision = 0;
		std::string referenceKey, shapeName;
	};

	/** Zero is uncovered. Nonzero ordinals address an exact frame-local 64-bit material ID. */
	inline uint64_t ResolveSidecarMaterial(uint32_t a_ordinal, std::span<const SidecarDraw> a_draws)
	{
		if (!a_ordinal || a_ordinal > a_draws.size())
			return 0;
		const auto& draw = a_draws[a_ordinal - 1];
		return draw.ordinal == a_ordinal ? draw.materialIdentity : 0;
	}

	/** Full-precision UV gradients; geometric TBN rows use finite FP16 storage. */
	struct SidecarSurface
	{
		std::array<float, 2> uv{}, ddx{}, ddy{};
		std::array<std::array<float, 3>, 3> basis{};
		float rasterDepth = 0;
		bool frontFacing = true;
	};

	inline bool SidecarDepthMatches(float a_writtenDepth, float a_finalDepth, bool a_depth24)
	{
		if (!std::isfinite(a_writtenDepth) || !std::isfinite(a_finalDepth) ||
			a_writtenDepth < 0 || a_writtenDepth > 1 || a_finalDepth < 0 || a_finalDepth > 1)
			return false;
		// D24 UNORM can quantize the original raster depth by half a representable unit.
		// D32 FLOAT must match the original interpolated SV_Position.z exactly.
		return a_depth24 ? std::abs(a_writtenDepth - a_finalDepth) <= 0.5f / 16777215.0f :
			a_writtenDepth == a_finalDepth;
	}

	/** GPU report layout shared with the DXC sidecar validation shader. */
	struct SidecarValidationReport
	{
		uint32_t validPixels = 0, invalidOrdinals = 0, invalidUV = 0, invalidGradients = 0;
		uint32_t invalidBasis = 0, invalidFacing = 0, depthMismatches = 0, coveredPixels = 0;
		uint32_t sampleOrdinal = 0, sampleFlags = 0, gradientMismatchQuads = 0, comparedQuads = 0;
		std::array<float, 4> uvDdx{}, ddyDepth{}, basis0{}, basis1{}, basis2{};
	};
	static_assert(sizeof(SidecarValidationReport) == 128);

	inline bool SidecarValidationClean(const SidecarValidationReport& report)
	{
		return report.coveredPixels != 0 && report.validPixels == report.coveredPixels &&
			!report.invalidOrdinals && !report.invalidUV && !report.invalidGradients &&
			!report.invalidBasis && !report.invalidFacing && !report.depthMismatches &&
			!report.gradientMismatchQuads;
	}
}

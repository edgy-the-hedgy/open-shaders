#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace NativePBR
{
	/** @brief Resolves a reflected shader constant from the engine's CPU-side float-offset table. */
	template <size_t N>
	std::optional<std::array<float, N>> ReadSnowShaderConstant(std::span<const float> a_constants,
		std::span<const uint8_t> a_offsets, size_t a_index)
	{
		if (a_index >= a_offsets.size() || !a_offsets[a_index])
			return std::nullopt;
		const size_t start = a_offsets[a_index];
		if (start > a_constants.size() || N > a_constants.size() - start)
			return std::nullopt;
		std::array<float, N> value{};
		std::copy_n(a_constants.begin() + start, N, value.begin());
		if (!std::ranges::all_of(value, [](float f) { return std::isfinite(f); }))
			return std::nullopt;
		return value;
	}
}

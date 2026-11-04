#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include "../../../package/Shaders/Common/NativeTraceSampling.hlsli"

namespace DX12Native
{
    /** @brief Separates native pixels, logical samples and horizontally packed checkerboard storage. */
    struct SignalGrid
    {
        std::array<uint32_t, 2> native{}, logical{}, packed{};
        bool checkerboard = false;
        uint32_t origin = 0;
        /** @brief Returns the exact logical producer coordinate of a stored sample. */
        constexpr std::array<uint32_t, 2> LogicalPixel(std::array<uint32_t, 2> pixel) const
        {
            return { checkerboard ? pixel[0] * 2u + ((pixel[1] + origin) & 1u) : pixel[0], pixel[1] };
        }
        /** @brief Selects a fixed central native receiver for each logical sample. */
        constexpr std::array<uint32_t, 2> NativePixel(std::array<uint32_t, 2> pixel) const
        {
            if (!logical[0] || !logical[1]) return {};
            return { uint32_t((uint64_t(pixel[0]) * 2u + 1u) * native[0] / (uint64_t(logical[0]) * 2u)),
                uint32_t((uint64_t(pixel[1]) * 2u + 1u) * native[1] / (uint64_t(logical[1]) * 2u)) };
        }
        /** @brief Rejects unsampled logical pixels instead of fabricating traced coverage. */
        constexpr std::optional<std::array<uint32_t, 2>> PackedPixel(std::array<uint32_t, 2> pixel) const
        {
            if (pixel[0] >= logical[0] || pixel[1] >= logical[1] ||
                (checkerboard && ((pixel[0] + pixel[1]) & 1u) != origin)) return std::nullopt;
            return std::array<uint32_t, 2>{ checkerboard ? pixel[0] / 2u : pixel[0], pixel[1] };
        }
    };

    /** @brief Retains odd edge blocks and declines checkerboard packing for odd logical widths. */
    constexpr SignalGrid MakeSignalGrid(std::array<uint32_t, 2> native, uint32_t resolution,
        bool checkerboard, uint32_t origin)
    {
        SignalGrid result{};
        result.native = native;
        const auto scale = 1u << (resolution > 2u ? 2u : resolution);
        result.logical = { native[0] / scale + uint32_t(native[0] % scale != 0u),
            native[1] / scale + uint32_t(native[1] % scale != 0u) };
        result.checkerboard = checkerboard && result.logical[0] && !(result.logical[0] & 1u) && result.logical[1];
        result.origin = result.checkerboard ? origin & 1u : 0u;
        result.packed = { result.checkerboard ? result.logical[0] / 2u : result.logical[0], result.logical[1] };
        return result;
    }

    /** @brief Finds the first requested RR sample that the real producer did not trace. */
    inline std::optional<std::array<uint32_t, 2>> FirstUntracedSample(const SignalGrid& grid,
        uint32_t mode, bool stochastic, uint32_t frame, uint32_t salt = 0)
    {
        if (mode == 0u) return std::nullopt;
        for (uint32_t y = 0; y < grid.packed[1]; ++y)
            for (uint32_t x = 0; x < grid.packed[0]; ++x) {
                const auto native = grid.NativePixel(grid.LogicalPixel({ x, y }));
                if (!NativeTraceSample(native[0], native[1], mode, stochastic, frame, salt)) return native;
            }
        return std::nullopt;
    }
}

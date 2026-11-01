#pragma once

// Pure CPU reference for the native RT per-pixel sampling contract.
// This file intentionally has no renderer or platform dependencies.
// Pass an explicit per-mode phase to the shader; do not derive phase from
// a wrapping 32-bit frame counter (3/9-frame cycles would repeat at wrap).
#include <array>
#include <algorithm>
#include <cstdint>

namespace DX12Native::RTSampling
{
    enum class Mode : uint32_t {
        Full = 0, ThreeQuarters, TwoThirds, Checkerboard,
        FourNinths, Quarter, Ninth, Sixteenth
    };

    struct Position { uint32_t x = 0, y = 0; };
    struct Extent { uint32_t width = 0, height = 0; };
    struct ModeInfo {
        uint32_t numerator, denominator, period, tileWidth, tileHeight;
    };
    struct Anchor {
        Position position{};
        bool valid = false;
    };

    constexpr std::array<ModeInfo, 8> kModes{{
        { 1, 1, 1, 1, 1 }, { 3, 4, 4, 2, 2 },
        { 2, 3, 3, 3, 1 }, { 1, 2, 2, 2, 2 },
        { 4, 9, 9, 3, 3 }, { 1, 4, 4, 2, 2 },
        { 1, 9, 9, 3, 3 }, { 1, 16, 16, 4, 4 }
    }};

    constexpr Mode ClampMode(uint32_t value)
    {
        return value < kModes.size() ? static_cast<Mode>(value) : Mode::Full;
    }

    constexpr ModeInfo Info(Mode mode)
    {
        return kModes[static_cast<uint32_t>(ClampMode(static_cast<uint32_t>(mode)))];
    }

    // Historic values were linear trace resolution, not percentages.
    // Never reinterpret them as the new enum's numeric values.
    constexpr Mode MigrateLegacy(uint32_t value)
    {
        switch (value) {
        case 0: return Mode::Full;
        case 1: return Mode::Quarter;
        case 2: return Mode::Sixteenth;
        case 3: return Mode::FourNinths;
        case 4: return Mode::Ninth;
        default: return Mode::Full;
        }
    }

    constexpr uint32_t Hash(uint32_t value)
    {
        value ^= value >> 16;
        value *= 0x7feb352du;
        value ^= value >> 15;
        value *= 0x846ca68bu;
        return value ^ (value >> 16);
    }

    // Use a monotonic 64-bit sequence on the CPU. HLSL receives only this
    // small phase value, so a 32-bit game-frame counter never breaks cycles.
    constexpr uint32_t GetPhase(uint64_t frame, Mode mode, bool temporal = true)
    {
        return temporal ? static_cast<uint32_t>(frame % Info(mode).period) : 0u;
    }

    // Phase is supplied explicitly (GetPhase), never calculated by reading
    // runtime frame state. Coordinates outside the render target are invalid.
    // Signal kinds deliberately share the same rank to maximize union overlap.
    constexpr bool IsSampled(Position pixel, uint32_t phase, Mode mode)
    {
        const uint32_t m = static_cast<uint32_t>(ClampMode(static_cast<uint32_t>(mode)));
        if (m == 0u) return true; // Full: no hash, permutation or phase.
        phase %= kModes[m].period;
        if (m == 3u) return ((pixel.x + pixel.y + phase) & 1u) == 0u;

        const auto& info = kModes[m];
        const uint32_t tileX = pixel.x / info.tileWidth;
        const uint32_t tileY = pixel.y / info.tileHeight;
        const uint32_t seed = Hash(tileX * 0x9e3779b9u ^ tileY * 0x85ebca6bu);
        const uint32_t x = pixel.x % info.tileWidth;
        const uint32_t y = pixel.y % info.tileHeight;

        if (m == 1u || m == 5u) {
            // Gray-ordered 2x2 indices preserve checkerboard parity.
            // Consequently 25% is always a subset of 50% and 75%.
            const uint32_t index = y == 0u ? x : 3u - x;
            return ((index + 2u * (seed & 1u) + phase) & 3u) < info.numerator;
        }
        if (m == 2u) return ((x + seed % 3u + phase) % 3u) < 2u;
        if (m == 4u || m == 6u)
            return ((5u * (y * 3u + x) + seed % 9u + phase) % 9u) < info.numerator;
        return ((5u * (y * 4u + x) + seed % 16u + phase) & 15u) == 0u;
    }

    constexpr Anchor GetTraceAnchor(Position pixel, Extent size, uint32_t phase, Mode mode)
    {
        Anchor out{};
        if (pixel.x >= size.width || pixel.y >= size.height) return out;
        if (IsSampled(pixel, phase, mode)) return { pixel, true };
        const auto info = Info(mode);
        const uint32_t startX = (pixel.x / info.tileWidth) * info.tileWidth;
        const uint32_t startY = (pixel.y / info.tileHeight) * info.tileHeight;
        const uint32_t maxX = std::min(info.tileWidth, size.width - startX);
        const uint32_t maxY = std::min(info.tileHeight, size.height - startY);
        uint32_t bestDistance = UINT32_MAX;
        for (uint32_t y = 0; y < maxY; ++y)
            for (uint32_t x = 0; x < maxX; ++x) {
                Position candidate{ startX + x, startY + y };
                if (!IsSampled(candidate, phase, mode)) continue;
                const uint32_t dx = candidate.x > pixel.x ? candidate.x - pixel.x : pixel.x - candidate.x;
                const uint32_t dy = candidate.y > pixel.y ? candidate.y - pixel.y : pixel.y - candidate.y;
                const uint32_t distance = dx * dx + dy * dy;
                if (distance < bestDistance) {
                    bestDistance = distance;
                    out = { candidate, true };
                }
            }
        // An incomplete edge tile can contain zero samples this phase.
        // Return invalid; the compositor must retain raster fallback.
        return out;
    }

    // O(width + height) edge adjustment rather than scanning every pixel.
    constexpr uint64_t GetExpectedSampleCount(Extent size, uint32_t phase, Mode mode)
    {
        if (!size.width || !size.height) return 0;
        const auto info = Info(mode);
        const uint32_t fullX = size.width / info.tileWidth;
        const uint32_t fullY = size.height / info.tileHeight;
        const uint32_t remX = size.width % info.tileWidth;
        const uint32_t remY = size.height % info.tileHeight;
        uint64_t count = uint64_t(fullX) * fullY * info.numerator;
        const auto edge = [&](uint32_t x0, uint32_t y0, uint32_t width, uint32_t height) {
            uint32_t samples = 0;
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x)
                    samples += IsSampled({ x0 + x, y0 + y }, phase, mode) ? 1u : 0u;
            return samples;
        };
        if (remX)
            for (uint32_t row = 0; row < fullY; ++row)
                count += edge(fullX * info.tileWidth, row * info.tileHeight, remX, info.tileHeight);
        if (remY)
            for (uint32_t col = 0; col < fullX; ++col)
                count += edge(col * info.tileWidth, fullY * info.tileHeight, info.tileWidth, remY);
        if (remX && remY)
            count += edge(fullX * info.tileWidth, fullY * info.tileHeight, remX, remY);
        return count;
    }
}

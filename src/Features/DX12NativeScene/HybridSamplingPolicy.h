#pragma once
#include <array>
#include <cstdint>

namespace DX12Native
{
    // Hybrid-Shadowed producer sampling; not an SDK-wide RR resolution selector.
    // The RR render extent is the LOGICAL extent. For checkerboard, only the
    // input signal storage width is halved, as required by the RR signal ABI.
    enum class HybridSamplingMode : uint32_t {
        Native = 0,
        NativeCheckerboard = 1,
        Quarter = 2,
        QuarterCheckerboard = 3
    };

    struct HybridSamplingLayout {
        std::array<uint32_t, 2> native{};
        std::array<uint32_t, 2> logical{}; // RR renderSize and reconstructed output
        std::array<uint32_t, 2> packed{};  // ray signal storage, not RR guides
        HybridSamplingMode requested = HybridSamplingMode::Native;
        HybridSamplingMode effective = HybridSamplingMode::Native;
        uint32_t checkerboardOrigin = 0;
        uint32_t representativePhase = 0;
        bool quarter = false;
        bool checkerboard = false;
        uint64_t tracedPixels = 0;
    };

    constexpr HybridSamplingLayout MakeHybridSamplingLayout(
        HybridSamplingMode requested, std::array<uint32_t, 2> native, uint32_t frame)
    {
        HybridSamplingLayout result{};
        result.native = native;
        result.requested = requested;
        result.quarter = requested == HybridSamplingMode::Quarter ||
                         requested == HybridSamplingMode::QuarterCheckerboard;
        const bool wantsCheckerboard =
            requested == HybridSamplingMode::NativeCheckerboard ||
            requested == HybridSamplingMode::QuarterCheckerboard;

        // Quarter = one sample per 2x2 native-pixel block (NOT width/4,
        // height/4). Round up odd native extents without dropping edge pixels.
        result.logical = result.quarter ?
            std::array<uint32_t, 2>{ native[0] / 2u + native[0] % 2u,
                                     native[1] / 2u + native[1] % 2u } : native;

        // Packed RR checkerboard requires an even logical width. For an odd
        // width, retain the requested spatial density and bypass checkerboard
        // rather than silently dropping the final column.
        result.checkerboard = wantsCheckerboard && result.logical[0] >= 2u &&
            (result.logical[0] % 2u == 0u) && result.logical[1] > 0u;
        result.effective = result.quarter ?
            (result.checkerboard ? HybridSamplingMode::QuarterCheckerboard : HybridSamplingMode::Quarter) :
            (result.checkerboard ? HybridSamplingMode::NativeCheckerboard : HybridSamplingMode::Native);
        result.packed = { result.checkerboard ? result.logical[0] / 2u : result.logical[0],
                          result.logical[1] };
        result.checkerboardOrigin = result.checkerboard ? frame % 2u : 0u;

        // Stable logical grid. Quarter visits 4 native subpixel positions over
        // 4 frames; quarter checkerboard visits all 4 over 8 frames, with the
        // active macroblock parity alternating on successive frames.
        result.representativePhase = result.quarter ?
            (result.checkerboard ? (frame / 2u) % 4u : frame % 4u) : 0u;
        result.tracedPixels = uint64_t(result.packed[0]) * result.packed[1];
        return result;
    }

    static_assert(MakeHybridSamplingLayout(HybridSamplingMode::Native,
        {1920u, 1080u}, 0u).tracedPixels == 1920ull * 1080ull);
    static_assert(MakeHybridSamplingLayout(HybridSamplingMode::NativeCheckerboard,
        {1920u, 1080u}, 0u).tracedPixels == 960ull * 1080ull);
    static_assert(MakeHybridSamplingLayout(HybridSamplingMode::Quarter,
        {1920u, 1080u}, 0u).tracedPixels == 960ull * 540ull);
    static_assert(MakeHybridSamplingLayout(HybridSamplingMode::QuarterCheckerboard,
        {1920u, 1080u}, 0u).tracedPixels == 480ull * 540ull);
    static_assert(MakeHybridSamplingLayout(HybridSamplingMode::QuarterCheckerboard,
        {1920u, 1080u}, 7u).representativePhase == 3u);
    static_assert(MakeHybridSamplingLayout(HybridSamplingMode::NativeCheckerboard,
        {1919u, 1080u}, 0u).effective == HybridSamplingMode::Native);
}

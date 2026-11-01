#pragma once
#include <array>
#include <cstdint>

namespace DX12Native
{
    // Legacy four-mode identifiers remain for saved-settings migration.
    enum class HybridSamplingMode : uint32_t {
        Native = 0, NativeCheckerboard = 1, Quarter = 2, QuarterCheckerboard = 3
    };
    struct HybridSamplingLayout {
        std::array<uint32_t, 2> native{};
        std::array<uint32_t, 2> logical{}; // RR logical render extent
        std::array<uint32_t, 2> packed{}; // storage for checkerboarded raw signals
        HybridSamplingMode requested = HybridSamplingMode::Native;
        HybridSamplingMode effective = HybridSamplingMode::Native;
        uint32_t checkerboardOrigin = 0, representativePhase = 0;
        uint32_t spatialScale = 1; // 1x/2x/4x native pixel block, matching SSGI dimensions
        bool checkerboard = false;
        uint64_t tracedPixels = 0;
    };
    // SSGI: full, half and quarter width AND height, with a separate
    // checkerboard flag. Retain odd-sized edge blocks (ceil rather than floor).
    constexpr HybridSamplingLayout MakeHybridSamplingLayout(
        uint32_t ssgiResolution, bool wantsCheckerboard,
        std::array<uint32_t, 2> native, uint32_t frame)
    {
        HybridSamplingLayout result{};
        result.native = native;
        result.spatialScale = 1u << (ssgiResolution > 2u ? 2u : ssgiResolution);
        const uint32_t scale = result.spatialScale;
        result.logical = {
            native[0] / scale + (native[0] % scale != 0u),
            native[1] / scale + (native[1] % scale != 0u) };
        // RR ABI expects horizontally packed checkerboard signals and full
        // logical-grid guides. Odd logical widths cannot be packed losslessly.
        result.checkerboard = wantsCheckerboard && result.logical[0] >= 2u &&
            result.logical[0] % 2u == 0u && result.logical[1] != 0u;
        result.requested = scale == 1u ?
            (wantsCheckerboard ? HybridSamplingMode::NativeCheckerboard : HybridSamplingMode::Native) :
            (wantsCheckerboard ? HybridSamplingMode::QuarterCheckerboard : HybridSamplingMode::Quarter);
        result.effective = scale == 1u ?
            (result.checkerboard ? HybridSamplingMode::NativeCheckerboard : HybridSamplingMode::Native) :
            (result.checkerboard ? HybridSamplingMode::QuarterCheckerboard : HybridSamplingMode::Quarter);
        result.packed = { result.checkerboard ? result.logical[0] / 2u : result.logical[0], result.logical[1] };
        result.checkerboardOrigin = result.checkerboard ? frame % 2u : 0u;
        result.representativePhase = (frame / (result.checkerboard ? 2u : 1u)) % (scale * scale);
        result.tracedPixels = uint64_t(result.packed[0]) * result.packed[1];
        return result;
    }
    // Compatibility only: the previous 'quarter' was a 2x2 native block.
    constexpr HybridSamplingLayout MakeHybridSamplingLayout(
        HybridSamplingMode mode, std::array<uint32_t, 2> native, uint32_t frame)
    {
        const bool coarse = mode == HybridSamplingMode::Quarter ||
            mode == HybridSamplingMode::QuarterCheckerboard;
        const bool checker = mode == HybridSamplingMode::NativeCheckerboard ||
            mode == HybridSamplingMode::QuarterCheckerboard;
        return MakeHybridSamplingLayout(coarse ? 1u : 0u, checker, native, frame);
    }
    constexpr HybridSamplingLayout MakeDirectSamplingLayout(
        bool checkerboard, std::array<uint32_t, 2> native, uint32_t frame)
    {
        return MakeHybridSamplingLayout(0u, checkerboard, native, frame);
    }
    // Direct DXR only ever uses native (mode 0) or native checkerboard (mode 5).
    constexpr uint32_t NativeTraceShaderMode(HybridSamplingMode mode)
    {
        return mode == HybridSamplingMode::NativeCheckerboard ? 5u : 0u;
    }
    // Deprecated compatibility helpers for old config migration.
    constexpr uint32_t RRLogicalReduction(HybridSamplingMode mode)
    {
        return mode == HybridSamplingMode::Quarter ||
            mode == HybridSamplingMode::QuarterCheckerboard ? 1u : 0u;
    }
    constexpr bool RRCheckerboard(HybridSamplingMode mode)
    {
        return mode == HybridSamplingMode::NativeCheckerboard ||
            mode == HybridSamplingMode::QuarterCheckerboard;
    }
    static_assert(NativeTraceShaderMode(HybridSamplingMode::NativeCheckerboard) == 5u);
    static_assert(MakeHybridSamplingLayout(0u, false, {1920, 1080}, 0).tracedPixels == 2073600ull);
    static_assert(MakeHybridSamplingLayout(0u, true, {1920, 1080}, 0).tracedPixels == 1036800ull);
    static_assert(MakeHybridSamplingLayout(1u, false, {1920, 1080}, 0).tracedPixels == 518400ull);
    static_assert(MakeHybridSamplingLayout(1u, true, {1920, 1080}, 0).tracedPixels == 259200ull);
    static_assert(MakeHybridSamplingLayout(2u, false, {1920, 1080}, 0).tracedPixels == 129600ull);
    static_assert(MakeHybridSamplingLayout(2u, true, {1920, 1080}, 0).tracedPixels == 64800ull);
    static_assert(MakeHybridSamplingLayout(2u, true, {1920, 1080}, 31).representativePhase == 15u);
    static_assert(MakeHybridSamplingLayout(0u, true, {1919, 1080}, 0).effective == HybridSamplingMode::Native);
}

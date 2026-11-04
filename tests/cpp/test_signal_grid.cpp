#include "Features/DX12NativeScene/SignalGrid.h"
#include <catch2/catch_test_macros.hpp>
#include "../../package/Shaders/Common/NativeDisplaySampling.hlsli"
#include <set>
#include <fstream>

using namespace DX12Native;

TEST_CASE("Native signal storage maps only to actual sampled receivers", "[SignalGrid]")
{
    for (const auto size : { std::array{ 32u, 18u }, std::array{ 31u, 17u }, std::array{ 1u, 1u } })
        for (uint32_t resolution = 0; resolution < 3; ++resolution)
            for (uint32_t origin = 0; origin < 2; ++origin) {
                const auto grid = MakeSignalGrid(size, resolution, true, origin);
                std::set<std::array<uint32_t, 2>> receivers;
                for (uint32_t y = 0; y < grid.packed[1]; ++y)
                    for (uint32_t x = 0; x < grid.packed[0]; ++x) {
                        const auto logical = grid.LogicalPixel({ x, y });
                        REQUIRE(logical[0] < grid.logical[0]);
                        REQUIRE(logical[1] < grid.logical[1]);
                        REQUIRE(grid.PackedPixel(logical) == std::array{ x, y });
                        const auto native = grid.NativePixel(logical);
                        REQUIRE(native[0] < size[0]);
                        REQUIRE(native[1] < size[1]);
                        REQUIRE(receivers.insert(native).second);
                        if (grid.checkerboard) REQUIRE_FALSE(grid.PackedPixel({ logical[0] ^ 1u, y }));
                    }
                REQUIRE(receivers.size() == uint64_t(grid.packed[0]) * grid.packed[1]);
            }
}

TEST_CASE("Signal grids retain odd edge blocks without phantom checkerboard samples", "[SignalGrid]")
{
    const auto odd = MakeSignalGrid({ 1919u, 1079u }, 0, true, 1);
    REQUIRE_FALSE(odd.checkerboard);
    REQUIRE(odd.packed == odd.native);
    const auto reduced = MakeSignalGrid({ 1919u, 1079u }, 1, true, 1);
    REQUIRE(reduced.logical == std::array{ 960u, 540u });
    REQUIRE(reduced.packed == std::array{ 480u, 540u });
    REQUIRE(reduced.origin == 1u);
    const auto empty = MakeSignalGrid({ 0u, 0u }, 0, true, 0);
    REQUIRE_FALSE(empty.checkerboard);
    REQUIRE_FALSE(empty.PackedPixel({ 0u, 0u }));
}

TEST_CASE("RR compatibility uses the producer's actual trace coordinates", "[SignalGrid]")
{
    SamplingCoverageCache cache;
    for (uint32_t mode = 0; mode < 5; ++mode)
        for (uint32_t resolution = 0; resolution < 3; ++resolution)
            for (bool stochastic : { false, true })
                for (uint32_t frame = 0; frame < 32; ++frame) {
                    const auto grid = MakeSignalGrid({ 32u, 18u }, resolution, true, frame);
                    const auto missing = FirstUntracedSample(grid, mode, stochastic, frame);
                    REQUIRE(cache.Check(grid, mode, stochastic, frame, 0) == missing);
                    if (missing) {
                        REQUIRE_FALSE(NativeTraceSample((*missing)[0], (*missing)[1], mode, stochastic, frame, 0));
                    } else {
                        for (uint32_t y = 0; y < grid.packed[1]; ++y)
                            for (uint32_t x = 0; x < grid.packed[0]; ++x) {
                                const auto pixel = grid.NativePixel(grid.LogicalPixel({ x, y }));
                                REQUIRE(NativeTraceSample(pixel[0], pixel[1], mode, stochastic, frame, 0));
                            }
                    }
                }
    REQUIRE_FALSE(FirstUntracedSample(MakeSignalGrid({ 1920u, 1080u }, 2, false, 0), 1, false, 0));
    REQUIRE(FirstUntracedSample(MakeSignalGrid({ 1920u, 1080u }, 1, false, 0), 1, false, 0));
    REQUIRE_FALSE(FirstUntracedSample(MakeSignalGrid({ 1919u, 1079u }, 1, false, 0), 1, false, 0));
    for (uint32_t mode = 0; mode < 5; ++mode) {
        const auto grid = MakeSignalGrid({ 31u, 17u }, 1, true, UINT32_MAX);
        REQUIRE(cache.Check(grid, mode, true, UINT32_MAX, 0x51b349d5u) ==
            FirstUntracedSample(grid, mode, true, UINT32_MAX, 0x51b349d5u));
    }
    std::ofstream report("native-rr-coverage.csv");
    report << "native_width,native_height,direct_mode,stochastic,rr_resolution,requested_checkerboard,logical_width,logical_height,packed_width,packed_height,effective_checkerboard,compatible_all_phases,supported_phases,total_phases,sdk_validated\n";
    for (const auto size : { std::array{ 1920u, 1080u }, std::array{ 1919u, 1079u } })
        for (uint32_t mode = 0; mode < 5; ++mode)
            for (bool stochastic : { false, true })
                for (uint32_t resolution = 0; resolution < 3; ++resolution)
                    for (bool checkerboard : { false, true }) {
                        const auto span = mode == 1 ? 2u : mode == 2 ? 4u : 3u;
                        uint32_t period = mode && stochastic ? span * span : 1u;
                        if (checkerboard && (period & 1u)) period *= 2u;
                        uint32_t supported = 0;
                        for (uint32_t frame = 0; frame < period; ++frame) {
                            const auto grid = MakeSignalGrid(size, resolution, checkerboard, frame);
                            const auto missing = FirstUntracedSample(grid, mode, stochastic, frame);
                            if (mode == 0) REQUIRE_FALSE(missing);
                            if (!stochastic && mode == 1 && size[0] == 1920 && resolution == 2) REQUIRE_FALSE(missing);
                            supported += uint32_t(!missing);
                        }
                        const auto layout = MakeSignalGrid(size, resolution, checkerboard, 0);
                        report << size[0] << ',' << size[1] << ',' << mode << ',' << stochastic << ',' << resolution << ',' << checkerboard << ','
                            << layout.logical[0] << ',' << layout.logical[1] << ',' << layout.packed[0] << ',' << layout.packed[1] << ',' << layout.checkerboard << ','
                            << (supported == period) << ',' << supported << ',' << period << ",not_run\n";
                    }
    REQUIRE(report.good());
}

TEST_CASE("Display reconstruction uses independent surface-safe selection", "[SignalGrid][Display]")
{
    REQUIRE(NativeDisplayRadius(0) == 0);
    REQUIRE(NativeDisplayRadius(2) == 4);
    REQUIRE(NativeDisplaySurface(7, 7, 0.2f, 100));
    REQUIRE_FALSE(NativeDisplaySurface(7, 8, 0.0f, 100));
    REQUIRE_FALSE(NativeDisplaySurface(0, 0, 0.0f, 100));
    REQUIRE_FALSE(NativeDisplaySurface(7, 7, 0.3f, 100));
    REQUIRE(NativeDisplayUseRR(1, 0, true, true));
    REQUIRE_FALSE(NativeDisplayUseRR(1, 1, true, true));
    REQUIRE_FALSE(NativeDisplayUseRR(1, 0, false, true));
    REQUIRE_FALSE(NativeDisplayUseRR(1, 0, true, false));
}

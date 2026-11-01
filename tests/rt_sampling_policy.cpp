// Standalone reference test: no changes to the normal CommunityShaders build.
// c++ -std=c++20 -O2 tests/rt_sampling_policy.cpp -o rt_sampling_test
#include "../src/Features/DX12NativeScene/RTSamplingPolicy.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

using namespace DX12Native::RTSampling;

int main()
{
    constexpr std::array<Extent, 7> sizes{{
        {1, 1}, {3, 2}, {7, 5}, {16, 16}, {17, 11}, {32, 19}, {127, 71}
    }};
    static_assert(MigrateLegacy(0) == Mode::Full);
    static_assert(MigrateLegacy(1) == Mode::Quarter);
    static_assert(MigrateLegacy(2) == Mode::Sixteenth);
    static_assert(MigrateLegacy(3) == Mode::FourNinths);
    static_assert(MigrateLegacy(4) == Mode::Ninth);

    for (uint32_t index = 0; index < 8; ++index) {
        const Mode mode = ClampMode(index);
        const auto info = Info(mode);
        for (const Extent size : sizes) {
            std::vector<uint32_t> coverage(uint64_t(size.width) * size.height);
            for (uint32_t frame = 0; frame < info.period; ++frame) {
                const uint32_t phase = GetPhase(frame, mode);
                uint64_t actual = 0;
                for (uint32_t y = 0; y < size.height; ++y)
                    for (uint32_t x = 0; x < size.width; ++x) {
                        const Position position{ x, y };
                        const bool sampled = IsSampled(position, phase, mode);
                        if (sampled) {
                            ++actual;
                            ++coverage[uint64_t(y) * size.width + x];
                        }
                        const Anchor anchor = GetTraceAnchor(position, size, phase, mode);
                        if (anchor.valid) {
                            assert(anchor.position.x < size.width && anchor.position.y < size.height);
                            assert(IsSampled(anchor.position, phase, mode));
                            if (sampled) {
                                assert(anchor.position.x == x && anchor.position.y == y);
                            }
                        }
                    }
                assert(actual == GetExpectedSampleCount(size, phase, mode));
                if (size.width % info.tileWidth == 0 && size.height % info.tileHeight == 0)
                    assert(actual == uint64_t(size.width) * size.height * info.numerator / info.denominator);
            }
            for (auto value : coverage) assert(value == info.numerator);
        }
    }

    // Nested 2x2 and 3x3 masks should minimize duplicate primary queries.
    for (uint32_t f = 0; f < 20; ++f)
        for (uint32_t y = 0; y < 19; ++y)
            for (uint32_t x = 0; x < 32; ++x) {
                const Position pixel{ x, y };
                if (IsSampled(pixel, GetPhase(f, Mode::Quarter), Mode::Quarter)) {
                    assert(IsSampled(pixel, GetPhase(f, Mode::ThreeQuarters), Mode::ThreeQuarters));
                    assert(IsSampled(pixel, GetPhase(f, Mode::Checkerboard), Mode::Checkerboard));
                }
                if (IsSampled(pixel, GetPhase(f, Mode::Ninth), Mode::Ninth))
                    assert(IsSampled(pixel, GetPhase(f, Mode::FourNinths), Mode::FourNinths));
            }

    // Temporal disabled: fixed phase. Full ignores phase entirely.
    for (uint32_t i = 0; i < 8; ++i) {
        const Mode mode = ClampMode(i);
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 8; ++x)
                assert(IsSampled({x, y}, GetPhase(99, mode, false), mode) ==
                       IsSampled({x, y}, 0, mode));
    }

    // 64-bit monotonic frame numbering survives 32-bit rollover for 3/9 cycles.
    constexpr uint64_t rollover = uint64_t{1} << 32;
    for (uint32_t i = 0; i < 8; ++i) {
        const Mode mode = ClampMode(i);
        const uint32_t period = Info(mode).period;
        for (uint64_t step = rollover - 8; step < rollover + 24; ++step)
            assert(GetPhase(step + period, mode) == GetPhase(step, mode));
    }
}

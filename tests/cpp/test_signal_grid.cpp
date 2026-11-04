#include "Features/DX12NativeScene/SignalGrid.h"
#include <catch2/catch_test_macros.hpp>
#include "../../package/Shaders/Common/NativeDisplaySampling.hlsli"
#include "../../package/Shaders/Common/NativeSignalSemantics.hlsli"
#include "../../package/Shaders/Common/NativeSceneComposition.hlsli"
#include "Features/DX12NativeScene/NativeSceneOwnership.h"
#include "../../package/Shaders/Common/NativeShadowMask.hlsli"
#include <limits>
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

TEST_CASE("Native GI sampling metadata preserves deterministic and temporal policies", "[SignalGrid]")
{
    for (uint32_t frame : { 0u, 1u, 2u, 63u, 64u, UINT32_MAX }) {
        REQUIRE(NativeGIPhase(frame, false) == 0u);
        REQUIRE(NativeGIPhase(frame, true) == frame);
        REQUIRE(MakeSignalGrid({32u,18u}, 1, true, frame).origin == (NativeGIPhase(frame, true) & 1u));
    }
    REQUIRE(NativeTraceHash(NativeGIPhase(1, true)) != NativeTraceHash(NativeGIPhase(2, true)));
}

TEST_CASE("Hybrid display covers native receivers without promoting raw samples", "[SignalGrid][Display]")
{
    for (const auto size : { std::array{32u,18u}, std::array{31u,17u} })
        for (uint32_t resolution = 0; resolution < 3; ++resolution)
            for (bool checkerboard : { false, true })
                for (uint32_t origin = 0; origin < 2; ++origin) {
                    const auto grid = MakeSignalGrid(size, resolution, checkerboard, origin);
                    for (uint32_t py = 0; py < size[1]; ++py) for (uint32_t px = 0; px < size[0]; ++px) {
                        const int cx = int(NativeDisplayLogical(px, size[0], grid.logical[0]));
                        const int cy = int(NativeDisplayLogical(py, size[1], grid.logical[1]));
                        const int radius = grid.checkerboard || grid.logical != size ? 1 : 0;
                        float best = 1e30f; bool covered = false;
                        for (int y = -radius; y <= radius; ++y) for (int x = -radius; x <= radius; ++x) {
                            const int lx = cx+x, ly = cy+y;
                            if (lx < 0 || ly < 0 || lx >= int(grid.logical[0]) || ly >= int(grid.logical[1])) continue;
                            const bool sampled = grid.PackedPixel({uint32_t(lx),uint32_t(ly)}).has_value();
                            const auto receiver = grid.NativePixel({uint32_t(lx),uint32_t(ly)});
                            REQUIRE(receiver[0] == NativeDisplayReceiver(uint32_t(lx),size[0],grid.logical[0]));
                            REQUIRE(receiver[1] == NativeDisplayReceiver(uint32_t(ly),size[1],grid.logical[1]));
                            const float dx = float(receiver[0])-float(px), dy = float(receiver[1])-float(py);
                            if (NativeDisplayPrefer(dx*dx+dy*dy,best,sampled)) { best=dx*dx+dy*dy; covered=true; }
                        }
                        REQUIRE(covered);
                    }
                }
}

TEST_CASE("Sparse display footprints cover only genuine producer candidates", "[SignalGrid][Display]")
{
    for (uint32_t mode=0; mode<5; ++mode)
        for (bool stochastic : {false,true})
            for (uint32_t frame=0; frame<18; ++frame)
                for (uint32_t py=0; py<17; ++py) for (uint32_t px=0; px<31; ++px) {
                    const int radius=int(NativeDisplaySearchRadius(mode,px,py,31,17));
                    bool fullTile=false;
                    for (int y=-radius;y<=radius;++y) for (int x=-radius;x<=radius;++x) {
                        const int sx=int(px)+x, sy=int(py)+y;
                        if (sx<0 || sy<0 || sx>=31 || sy>=17) continue;
                        const bool actual=NativeTraceSample(uint32_t(sx),uint32_t(sy),mode,stochastic,frame,0);
                        fullTile |= actual;
                    }
                    REQUIRE(fullTile);
                    if (mode==0) REQUIRE(radius==0);
                }
    for (uint32_t mask=0; mask<4; ++mask) {
        REQUIRE(NativeDisplayUseRR(mask,0,true,true)==bool(mask&1u));
        REQUIRE(NativeDisplayUseRR(mask,1,true,true)==bool(mask&2u));
        REQUIRE_FALSE(NativeDisplayUseRR(mask,0,false,true));
    }
}

TEST_CASE("RR radiance validity does not infer hit distance from Direct alpha", "[SignalGrid][SignalContract]")
{
    REQUIRE(NativeDirectAlpha(true) == 0.0f);
    REQUIRE(NativeDirectAlpha(false) == -1.0f);
    REQUIRE(NativeRRChannelValid(0));
    REQUIRE(NativeRRChannelValid(65504));
    REQUIRE_FALSE(NativeRRChannelValid(-1));
    REQUIRE_FALSE(NativeRRChannelValid(std::numeric_limits<float>::infinity()));
    REQUIRE_FALSE(NativeRRChannelValid(std::numeric_limits<float>::quiet_NaN()));

}

TEST_CASE("Final native scene composition selects independent producer outputs", "[NativeScene]")
{
    for (uint32_t mask=0;mask<8;++mask) {
        const bool d=mask&1u,i=mask&2u,h=mask&4u;
        const float value=NativeSceneChannel(10,20,3,4,d,i,h,1);
        REQUIRE(value==(d ? 20.0f : 10.0f)+(i ? 3.0f : 0)+(h ? 4.0f : 0));
        REQUIRE(NativeSceneChannel(10,20,3,4,d,i,h,0)==10+(i ? 3.0f : 0)+(h ? 4.0f : 0));
    }
    REQUIRE(NativeSceneChannel(10,20,0,0,true,false,false,0.5f)==15);
}

TEST_CASE("Shared scene leases wait for every native consumer", "[NativeScene]")
{
    REQUIRE_FALSE(NativeSceneReusable(true,5,3,8,7));
    REQUIRE_FALSE(NativeSceneReusable(false,2,3,8,7));
    REQUIRE_FALSE(NativeSceneReusable(false,5,3,6,7));
    REQUIRE(NativeSceneReusable(false,5,3,8,7));
    REQUIRE_FALSE(NativeSceneReusable(false,UINT64_MAX,3,8,7));
    REQUIRE_FALSE(NativeSceneReusable(false,5,3,UINT64_MAX,7));
    REQUIRE(NativeSceneBudgetAllows(100,20,120));
    REQUIRE_FALSE(NativeSceneBudgetAllows(100,21,120));
    REQUIRE_FALSE(NativeSceneBudgetAllows(121,0,120));
    REQUIRE_FALSE(NativeSceneBudgetAllows(UINT64_MAX,1,UINT64_MAX));
}

TEST_CASE("RR opt-in is bounded by genuine producer signals", "[NativeScene][SignalContract]")
{
    for (uint32_t produced=0;produced<16;++produced) for (uint32_t selected=0;selected<16;++selected) {
        REQUIRE(NativeRRRequestedSignals(false,produced,selected)==0u);
        REQUIRE((NativeRRRequestedSignals(true,produced,selected)&~produced)==0u);
        for (uint32_t signal=0;signal<4;++signal)
            REQUIRE(bool(NativeRRRequestedSignals(true,produced,selected)&(1u<<signal))==bool((produced&selected)&(1u<<signal)));
    }
}

TEST_CASE("Native scene allocation estimates cannot wrap the budget", "[NativeScene]")
{
    uint64_t required=4;
    REQUIRE(NativeSceneBudgetAdd(required,8,2,20));
    REQUIRE(required==20);
    REQUIRE_FALSE(NativeSceneBudgetAdd(required,1,1,20));
    REQUIRE(required==20);
    required=0;
    REQUIRE_FALSE(NativeSceneBudgetAdd(required,UINT64_MAX,2,UINT64_MAX));
    REQUIRE(required==0);
    REQUIRE_FALSE(NativeSceneBudgetAdd(required,1,0,20));
}

TEST_CASE("Prepared RT shadow-mask contract separates visibility and coverage", "[NativeShadow]")
{
    for (bool receiver:{false,true}) for (bool geometry:{false,true}) for (bool blocked:{false,true}) {
        const bool valid=NativeShadowValid(receiver,geometry,10.0f);
        REQUIRE(valid==(receiver&&geometry));
        REQUIRE(NativeShadowVisibility(valid,blocked)==(valid&&blocked ? 0.0f : 1.0f));
    }
    REQUIRE_FALSE(NativeShadowValid(true,true,0));
    REQUIRE_FALSE(NativeShadowValid(true,true,std::numeric_limits<float>::quiet_NaN()));
    REQUIRE_FALSE(NativeShadowValid(true,true,std::numeric_limits<float>::infinity()));
    REQUIRE_FALSE(NativeShadowValid(true,true,60001));
    for (uint32_t size:{1u,31u,32u,1919u}) for (uint32_t pixel=0;pixel<size;++pixel) {
        const float coordinate=NativeShadowClip(pixel,size);
        REQUIRE(coordinate>-1.0f);
        REQUIRE(coordinate<1.0f);
        REQUIRE(std::abs(coordinate+NativeShadowClip(size-1-pixel,size))<1e-5f);
    }
}

TEST_CASE("Independent shadow image layer preserves bypass and receiver boundaries", "[NativeShadow]")
{
    for (float visibility : {0.0f, 0.25f, 1.0f}) {
        REQUIRE(NativeShadowFactor(visibility,0,1)==1);
        REQUIRE(NativeShadowFactor(visibility,1,0)==1);
        REQUIRE(NativeShadowFactor(visibility,1,1)==visibility);
        REQUIRE(NativeShadowFactor(visibility,0.5f,0.5f)==1-(1-visibility)*0.25f);
    }
    REQUIRE(NativeShadowFactor(1,1,1)==1);
    REQUIRE(NativeShadowReceiverMatches(4,2));
    REQUIRE_FALSE(NativeShadowReceiverMatches(4.01f,2));
    REQUIRE_FALSE(NativeShadowReceiverMatches(-1,2));
    REQUIRE_FALSE(NativeShadowReceiverMatches(std::numeric_limits<float>::quiet_NaN(),2));
    REQUIRE_FALSE(NativeShadowReceiverMatches(std::numeric_limits<float>::infinity(),2));
    const float direct = 3, indirect = 2, raster = 1;
    const float original = NativeSceneChannel(raster,direct,indirect,0,true,true,false,1);
    REQUIRE(original==5);
    REQUIRE(original*NativeShadowFactor(0,0.5f,1)==2.5f);
}

TEST_CASE("Local shadow visibility follows contributing light energy", "[NativeShadow]")
{
    const float bright = NativeShadowLightWeight(4,0.5f,1);
    const float dim = NativeShadowLightWeight(1,0.5f,1);
    REQUIRE(NativeShadowWeightedVisibility(bright,bright+dim)==0.8f);
    REQUIRE(NativeShadowWeightedVisibility(dim,bright+dim)==0.2f);
    REQUIRE(NativeShadowWeightedVisibility(0,bright+dim)==0);
    REQUIRE(NativeShadowWeightedVisibility(bright+dim,bright+dim)==1);
    REQUIRE(NativeShadowWeightedVisibility(0,0)==1);
    REQUIRE(NativeShadowLightWeight(100,1,0)==0);
    REQUIRE(NativeShadowLightWeight(100,0,1)==0);
    REQUIRE(NativeShadowHistoryValue(0,1,0.7f)<0.13f);
    REQUIRE(NativeShadowHistoryValue(1,0,0.7f)>0.87f);
    REQUIRE(NativeShadowHistoryValue(0.5f,0.6f,0)==0.5f);
    REQUIRE(NativeShadowHistoryCompatible(11,10,2,2,3,3));
    REQUIRE_FALSE(NativeShadowHistoryCompatible(12,10,2,2,3,3));
    REQUIRE_FALSE(NativeShadowHistoryCompatible(11,10,2,1,3,3));
    REQUIRE_FALSE(NativeShadowHistoryCompatible(11,10,2,2,3,4));
    REQUIRE_FALSE(NativeShadowHistoryCompatible(0,UINT32_MAX,2,2,3,3));
}

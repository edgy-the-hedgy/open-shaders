#include "Features/NativeCubemaps/NativeCubemapPolicy.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace NativeCubemaps::Policy;

TEST_CASE("Native cubemap acquisition is bounded and wrap-safe", "[dx12][cubemaps]")
{
    REQUIRE(CaptureDue(UINT32_MAX, 0));
    REQUIRE_FALSE(CaptureDue(100, 100));
    REQUIRE_FALSE(CaptureDue(100, 105));
    REQUIRE(CaptureDue(100, 106));
    REQUIRE(CaptureDue(UINT32_MAX - 2, 3));
    REQUIRE(CaptureDue(100, 101, 1));
    REQUIRE_FALSE(CaptureDue(100, 159, 60));
    REQUIRE(CaptureDue(100, 160, 60));
    REQUIRE(CaptureDue(100, 160, 999));
    REQUIRE_FALSE(CaptureDue(100, 100, 0));
}

TEST_CASE("Native cubemap history rejects incompatible and stale observations", "[dx12][cubemaps]")
{
    REQUIRE(CanReuseHistory(true, 6, 0));
    REQUIRE(CanReuseHistory(true, 120, 128.0f * 128.0f));
    REQUIRE_FALSE(CanReuseHistory(false, 6, 0));
    REQUIRE_FALSE(CanReuseHistory(true, 0, 0));
    REQUIRE_FALSE(CanReuseHistory(true, 121, 0));
    REQUIRE_FALSE(CanReuseHistory(true, 6, 129.0f * 129.0f));
    REQUIRE_FALSE(CanReuseHistory(true, 6, -1));
    REQUIRE_FALSE(CanReuseHistory(true, 6, std::numeric_limits<float>::quiet_NaN()));
}

TEST_CASE("Environment publications expire and reject incompatible generations", "[dx12][cubemaps]")
{
    REQUIRE(PublicationFresh(true, 10, 10));
    REQUIRE(PublicationFresh(true, 10, 130));
    REQUIRE_FALSE(PublicationFresh(true, 10, 131));
    REQUIRE_FALSE(PublicationFresh(false, 10, 10));
    REQUIRE(PublicationFresh(true, UINT32_MAX - 2, 3));
    REQUIRE_FALSE(PublicationFresh(true, 10, 9));
}

TEST_CASE("Environment scheduler visits every mip before publication", "[dx12][cubemaps]")
{
    for (bool reflection : { false, true }) {
        ProcessingCursor cursor;
        REQUIRE(cursor.phase == ProcessingPhase::History);
        cursor.Advance(reflection);
        uint32_t base = 0, reflected = 0, steps = 0;
        while (cursor.phase != ProcessingPhase::Complete && steps++ < 2 * kMips) {
            REQUIRE(cursor.EndMip() > cursor.mip);
            REQUIRE(cursor.EndMip() - cursor.mip <= kMipsPerStep);
            auto& count = cursor.phase == ProcessingPhase::Base ? base : reflected;
            REQUIRE(cursor.mip == count);
            count = cursor.EndMip();
            cursor.Advance(reflection);
        }
        REQUIRE(cursor.phase == ProcessingPhase::Complete);
        REQUIRE(base == kMips);
        REQUIRE(reflected == (reflection ? kMips : 0u));
    }
}

TEST_CASE("Flat water reflection history rejects incompatible publications", "[dx12][cubemaps]")
{
    WaterHistoryIdentity old{10,20,1920,1080,960,540,12,1,1,123,4,4,2,8,0.05f,true};
    auto now=old;now.engineFrame++;now.pluginFrame++;now.reset=false;
    REQUIRE(now.CanReuse(old));
    now.radius=0.1f;REQUIRE_FALSE(now.CanReuse(old));now.radius=old.radius;
    now.quality=3;REQUIRE_FALSE(now.CanReuse(old));now.quality=old.quality;
    now.environmentRevision++;REQUIRE_FALSE(now.CanReuse(old));now.environmentRevision=old.environmentRevision;
    now.scope++;REQUIRE_FALSE(now.CanReuse(old));now.scope=old.scope;
    now.reset=true;REQUIRE_FALSE(now.CanReuse(old));now.reset=false;
    now.engineFrame++;REQUIRE_FALSE(now.CanReuse(old));
}
TEST_CASE("Ordinary clock evolution does not invalidate cubemap generations", "[dx12][cubemaps]")
{
    REQUIRE_FALSE(ClockDiscontinuity(100.0f,100.02f));
    REQUIRE(ClockDiscontinuity(100.0f,101.0f));
    REQUIRE(ClockDiscontinuity(100.0f,99.0f));
}

TEST_CASE("Cubemap face freshness cannot be extended by refreshing just one face", "[dx12][cubemaps]")
{
    CubeFaceHistory faces;
    faces.Observe(0x1fu,10);
    REQUIRE_FALSE(faces.Fresh(10));
    faces.Observe(0x20u,11);
    REQUIRE(faces.Fresh(130));
    faces.Observe(1u,131);
    REQUIRE_FALSE(faces.Fresh(131));
    faces.Observe(0x3eu,131);
    REQUIRE(faces.Fresh(131));
    faces={};faces.Observe(0x3fu,UINT32_MAX-1);
    REQUIRE(faces.Fresh(1));
}

TEST_CASE("Cubemap lighting discontinuities are separate from normal exposure evolution", "[dx12][cubemaps]")
{
    REQUIRE_FALSE(LightingDiscontinuity(1.0f,1.1f));
    REQUIRE(LightingDiscontinuity(1.0f,2.0f));
    REQUIRE(LightingDiscontinuity(1.0f,0.5f));
    REQUIRE_FALSE(LightingDiscontinuity(0.0f,0.0001f));
    REQUIRE(LightingDiscontinuity(0.0f,1.0f));
    REQUIRE(LightingDiscontinuity(1.0f,std::numeric_limits<float>::quiet_NaN()));
}

TEST_CASE("Water SSR requires current-frame main-view water evidence", "[dx12][cubemaps][water]")
{
    WaterDrawEvidence evidence;
    REQUIRE_FALSE(evidence.Matches(1,2,3));
    evidence.Observe(1,2,3);
    REQUIRE(evidence.Matches(1,2,3));
    REQUIRE_FALSE(evidence.Matches(2,2,3));
    REQUIRE_FALSE(evidence.Matches(1,3,3));
    REQUIRE_FALSE(evidence.Matches(1,2,4));
    REQUIRE_FALSE(WaterCellEligible(0,false,true,true));
    REQUIRE_FALSE(WaterCellEligible(3,true,false,false));
    REQUIRE(WaterCellEligible(3,true,true,false));
    REQUIRE(WaterCellEligible(3,true,false,true)); // Modded interior water.
    REQUIRE(WaterCellEligible(3,false,false,false)); // Adjacent exterior/LOD.
    evidence={};
    REQUIRE_FALSE(evidence.Matches(1,2,3));
}

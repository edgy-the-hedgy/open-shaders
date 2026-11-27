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

#include "Features/NativeIBL/NativeIBLPolicy.h"
#include <catch2/catch_test_macros.hpp>

using NativeIBL::Policy::EnvironmentContentKey;

TEST_CASE("Native IBL history keys distinguish independently changing environment inputs", "[dx12][ibl]")
{
    constexpr auto key = EnvironmentContentKey(1, 2, 0, 2, 2, 5);
    static_assert(key != 0);
    REQUIRE(EnvironmentContentKey(1, 2, 0, 2, 2, 5) == key);
    REQUIRE(EnvironmentContentKey(2, 2, 0, 2, 2, 5) != key);
    REQUIRE(EnvironmentContentKey(1, 3, 0, 2, 2, 5) != key);
    REQUIRE(EnvironmentContentKey(1, 2, 1, 2, 2, 5) != key);
    REQUIRE(EnvironmentContentKey(1, 2, 0, 3, 2, 5) != key);
    REQUIRE(EnvironmentContentKey(1, 2, 0, 2, 0, 5) != key);
    REQUIRE(EnvironmentContentKey(1, 2, 0, 2, 2, 0) != key);
    REQUIRE(EnvironmentContentKey(1, 2, 0, 2, 2, 6) != key);
    REQUIRE(EnvironmentContentKey(0, 2, 0, 2, 2, 5) == 0);
    REQUIRE(EnvironmentContentKey(1, 0, 0, 2, 2, 5) == 0);
    REQUIRE(EnvironmentContentKey(1, 2, 0, 0, 2, 5) == 0);
}

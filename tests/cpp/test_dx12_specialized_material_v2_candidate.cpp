// Tests the shared approved CPU ABI rather than a duplicate test-only model.
#include "Features/NativePBR/SpecializedMaterialAbi.h"
#include <catch2/catch_test_macros.hpp>
using namespace NativePBR::SpecializedMaterial;

TEST_CASE("Specialized v2 preserves independent material properties", "[dx12][materials][specialized-v2]")
{
    constexpr auto all = Coat | SSS | Front | Back | Fuzz | Glint;
    const auto d = Publish(all, all | (1u << 2u), 0, 1);
    REQUIRE(d.ordinal == 0u);
    for (uint32_t bit : { Coat, SSS, Front, Back, Fuzz, Glint })
        REQUIRE(Property(d, bit) == State::Available);
    REQUIRE(Property(d, 1u << 2u) == State::Absent);
    REQUIRE(Property(d, 1u << 3u) == State::Unknown);
    // A zero payload is not the same thing as missing data.
    const Payload zero{};
    REQUIRE(zero.coatColorStrength[0] == 0u);
    REQUIRE(Property(d, Coat) == State::Available);
}

TEST_CASE("Specialized v2 overflow retains presence and known absence", "[dx12][materials][specialized-v2]")
{
    const auto d = Publish(Coat | Fuzz | Glint, Coat | Fuzz | SSS, 8, 8);
    REQUIRE(d.ordinal == NoRecord);
    REQUIRE(d.present == (Coat | Fuzz | Glint));
    REQUIRE(d.valid == SSS);
    REQUIRE(Property(d, Coat) == State::Unavailable);
    REQUIRE(Property(d, Fuzz) == State::Unavailable);
    REQUIRE(Property(d, Glint) == State::Unavailable);
    REQUIRE(Property(d, SSS) == State::Absent);
    REQUIRE(Property(d, Front) == State::Unknown);
    REQUIRE(Publish(Coat, Coat, NoRecord, UINT32_MAX).ordinal == NoRecord);
    REQUIRE(Publish(Coat, Coat, 0, 0).valid == 0u);
}

TEST_CASE("Specialized v2 needs no allocation for unknown or absent properties", "[dx12][materials][specialized-v2]")
{
    const auto d = Publish(Fuzz | (1u << 31u), Coat | (1u << 31u), 0, 4);
    REQUIRE(d.ordinal == NoRecord);
    REQUIRE(d.present == Fuzz);
    REQUIRE(d.valid == Coat);
    REQUIRE(Property(d, Fuzz) == State::Unavailable);
    REQUIRE(Property(d, Coat) == State::Absent);
}

TEST_CASE("Specialized v2 sizes aligned output and bounds raw addresses", "[dx12][materials][specialized-v2]")
{
    constexpr auto fullHD = Plan(1920, 1080);
    static_assert(fullHD.valid);
    static_assert(fullHD.capacity == 414720);
    REQUIRE((fullHD.directoryBytes + fullHD.payloadBytes) * 4u == 258785280u);
    const auto odd = Plan(1921, 1080);
    REQUIRE(odd.valid);
    REQUIRE(odd.pitch == 1928u);
    REQUIRE(odd.capacity == odd.pixels / 5u);
    const auto tiny = Plan(1, 1);
    REQUIRE(tiny.valid);
    REQUIRE(tiny.capacity == 1u);
    REQUIRE_FALSE(Plan(0, 1080).valid);
    REQUIRE_FALSE(Plan(1920, 0).valid);
    REQUIRE_FALSE(Plan(UINT32_MAX, UINT32_MAX).valid);
    REQUIRE_FALSE(Plan(16384, 16384).valid);
}

TEST_CASE("Specialized v2 validates publication independently of base", "[dx12][materials][specialized-v2]")
{
    constexpr auto p = Plan(1921, 1080);
    REQUIRE(ValidPublicationLayout(Version, 1921, 1080, p.pitch, p.capacity, p.directoryBytes, p.payloadBytes));
    REQUIRE(ValidPublicationLayout(Version, 1921, 1080, p.pitch, 0, p.directoryBytes, 0));
    REQUIRE_FALSE(ValidPublicationLayout(1, 1921, 1080, p.pitch, p.capacity, p.directoryBytes, p.payloadBytes));
    REQUIRE_FALSE(ValidPublicationLayout(Version, 1921, 1080, p.pitch, p.capacity+1, p.directoryBytes, p.payloadBytes));
    REQUIRE_FALSE(ValidPublicationLayout(Version, 1921, 1080, p.pitch, p.capacity, p.directoryBytes-1, p.payloadBytes));
    REQUIRE_FALSE(ValidPublicationLayout(Version, 1921, 1080, p.pitch, p.capacity, p.directoryBytes, p.payloadBytes-1));
    const auto invalid = Sanitize({ 9, Coat, Coat|Fuzz }, 9);
    REQUIRE(invalid.ordinal == NoRecord);
    REQUIRE(Property(invalid, Coat) == State::Unavailable);
    REQUIRE(Property(invalid, Fuzz) == State::Absent);
}

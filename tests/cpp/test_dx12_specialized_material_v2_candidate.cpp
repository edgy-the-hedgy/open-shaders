// Test-only reference for docs/native-specialized-material-v2-proposal.md.
// Not a runtime ABI definition or authorization to migrate any consumer.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace
{
    constexpr uint32_t NoRecord = UINT32_MAX;
    constexpr uint32_t KnownBits = (1u << 15u) - 1u;
    constexpr uint32_t Coat = 1u << 0u;
    constexpr uint32_t SSS = 1u << 4u;
    constexpr uint32_t Front = 1u << 6u;
    constexpr uint32_t Back = 1u << 7u;
    constexpr uint32_t Fuzz = 1u << 8u;
    constexpr uint32_t Glint = 1u << 11u;

    struct Directory
    {
        uint32_t ordinal = NoRecord;
        uint32_t present = 0;
        uint32_t valid = 0;
    };

    struct Payload
    {
        std::array<uint32_t, 2> coatColorStrength;
        std::array<uint32_t, 2> coatNormalRoughnessF0;
        std::array<uint32_t, 2> interlayer;
        std::array<uint32_t, 2> subsurface;
        std::array<uint32_t, 4> thinSurface;
        std::array<uint32_t, 2> transmission;
        std::array<uint32_t, 2> fuzz;
        std::array<uint32_t, 2> fuzzRoughnessAnisotropyTangent;
        std::array<float, 4> glint;
        uint32_t eyeNormal;
        uint32_t flags;
    };

    static_assert(sizeof(Directory) == 12);
    static_assert(offsetof(Directory, ordinal) == 0);
    static_assert(offsetof(Directory, present) == 4);
    static_assert(offsetof(Directory, valid) == 8);
    static_assert(sizeof(Payload) == 96);
    static_assert(offsetof(Payload, coatColorStrength) == 0);
    static_assert(offsetof(Payload, coatNormalRoughnessF0) == 8);
    static_assert(offsetof(Payload, interlayer) == 16);
    static_assert(offsetof(Payload, subsurface) == 24);
    static_assert(offsetof(Payload, thinSurface) == 32);
    static_assert(offsetof(Payload, transmission) == 48);
    static_assert(offsetof(Payload, fuzz) == 56);
    static_assert(offsetof(Payload, fuzzRoughnessAnisotropyTangent) == 64);
    static_assert(offsetof(Payload, glint) == 72);
    static_assert(offsetof(Payload, eyeNormal) == 88);
    static_assert(offsetof(Payload, flags) == 92);

    enum class State { Unknown, Absent, Unavailable, Available };

    constexpr State Property(Directory d, uint32_t bit)
    {
        return (d.valid & bit) ?
            ((d.present & bit) ? State::Available : State::Absent) :
            ((d.present & bit) ? State::Unavailable : State::Unknown);
    }

    // Model only: the allocator supplies a frame-local ordinal, not a
    // persistent object identifier. A missing payload must not erase evidence.
    constexpr Directory Publish(uint32_t present, uint32_t valid,
        uint32_t reservedOrdinal, uint32_t capacity)
    {
        Directory d{ NoRecord, present & KnownBits, valid & KnownBits };
        if ((d.present & d.valid) == 0u)
            return d;
        if (reservedOrdinal == NoRecord || reservedOrdinal >= capacity) {
            d.valid &= ~d.present;
            return d;
        }
        d.ordinal = reservedOrdinal;
        return d;
    }

    struct Layout
    {
        uint64_t pitch = 0;
        uint64_t pixels = 0;
        uint64_t capacity = 0;
        uint64_t directoryBytes = 0;
        uint64_t payloadBytes = 0;
        bool valid = false;
    };

    // Candidate 20% policy; separate raw buffers must fit 32-bit byte
    // addressing. This is logical allocation size, not actual DXGI cost.
    constexpr Layout Plan(uint32_t width, uint32_t height)
    {
        if (!width || !height)
            return {};
        const uint64_t pitch = (uint64_t(width) + 7u) & ~uint64_t(7u);
        if (pitch > UINT32_MAX)
            return {};
        const uint64_t pixels = pitch * uint64_t(height);
        if (pixels > UINT32_MAX / sizeof(Directory))
            return {};
        const uint64_t capacity = pixels / 5u;
        if (capacity > UINT32_MAX / sizeof(Payload))
            return {};
        return { pitch, pixels, capacity, pixels * sizeof(Directory),
            capacity * sizeof(Payload), true };
    }
}

TEST_CASE("Candidate specialized v2 preserves independent material properties", "[dx12][materials][candidate]")
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

TEST_CASE("Candidate specialized v2 overflow retains presence and known absence", "[dx12][materials][candidate]")
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

TEST_CASE("Candidate specialized v2 needs no allocation for unknown or absent properties", "[dx12][materials][candidate]")
{
    const auto d = Publish(Fuzz | (1u << 31u), Coat | (1u << 31u), 0, 4);
    REQUIRE(d.ordinal == NoRecord);
    REQUIRE(d.present == Fuzz);
    REQUIRE(d.valid == Coat);
    REQUIRE(Property(d, Fuzz) == State::Unavailable);
    REQUIRE(Property(d, Coat) == State::Absent);
}

TEST_CASE("Candidate specialized v2 sizes aligned output and bounds raw addresses", "[dx12][materials][candidate]")
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

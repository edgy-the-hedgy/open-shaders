#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace NativePBR::SpecializedMaterial
{
    // Independent of raster ABI v6 and the primary material publication.
    inline constexpr uint32_t Version = 2;
    inline constexpr uint32_t NoRecord = UINT32_MAX;
    inline constexpr uint32_t KnownBits = (1u << 15u) - 1u;
    enum PropertyBit : uint32_t
    {
        Coat = 1u << 0u, ColoredCoat = 1u << 1u, CoatNormal = 1u << 2u,
        Interlayer = 1u << 3u, SSS = 1u << 4u, ThinSurface = 1u << 5u,
        Front = 1u << 6u, Back = 1u << 7u, Fuzz = 1u << 8u,
        Anisotropy = 1u << 9u, Tangent = 1u << 10u, Glint = 1u << 11u,
        EyeNormal = 1u << 12u, Transmission = 1u << 13u, SpecularControls = 1u << 14u
    };
    inline constexpr uint32_t FoliageFlag = 1u;
    inline constexpr uint32_t NegativeHandednessFlag = 2u;
    inline constexpr uint32_t KnownFlags = FoliageFlag | NegativeHandednessFlag;

    struct Directory
    {
        uint32_t ordinal = NoRecord;
        uint32_t present = 0;
        uint32_t valid = 0;
    };

    struct Payload
    {
        std::array<uint32_t, 2> coatColorStrength{};
        std::array<uint32_t, 2> coatNormalRoughnessF0{};
        std::array<uint32_t, 2> interlayer{};
        std::array<uint32_t, 2> subsurface{};
        std::array<uint32_t, 4> thinSurface{};
        std::array<uint32_t, 2> transmission{};
        std::array<uint32_t, 2> fuzz{};
        std::array<uint32_t, 2> fuzzRoughnessAnisotropyTangent{};
        std::array<float, 4> glint{};
        uint32_t eyeNormal = 0;
        uint32_t flags = 0;
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
    inline constexpr uint32_t DirectoryStride = sizeof(Directory);
    inline constexpr uint32_t PayloadStride = sizeof(Payload);
    // Counter words: admitted records, overflowed pixels, followed by
    // fifteen independent overflow counters. Reset all before dispatch.
    inline constexpr uint32_t CounterWords = 17;

    enum class State { Unknown, Absent, Unavailable, Available };
    constexpr State Property(Directory d, uint32_t bit)
    {
        return (d.valid & bit) ?
            ((d.present & bit) ? State::Available : State::Absent) :
            ((d.present & bit) ? State::Unavailable : State::Unknown);
    }
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
        uint64_t pitch = 0, pixels = 0, capacity = 0;
        uint64_t directoryBytes = 0, payloadBytes = 0;
        bool valid = false;
        constexpr uint64_t LogicalBytes() const { return directoryBytes + payloadBytes; }
    };

    // Initial approved 20% limit only. Separate raw buffers must fit
    // 32-bit byte addressing; actual DXGI allocation costs are additional.
    constexpr Layout Plan(uint32_t width, uint32_t height)
    {
        if (!width || !height)
            return {};
        const uint64_t pitch = (uint64_t(width) + 7u) & ~uint64_t(7u);
        if (pitch > UINT32_MAX)
            return {};
        const uint64_t pixels = pitch * uint64_t(height);
        if (pixels > UINT32_MAX / DirectoryStride)
            return {};
        const uint64_t capacity = pixels / 5u;
        if (capacity > UINT32_MAX / PayloadStride)
            return {};
        return { pitch, pixels, capacity, pixels * DirectoryStride,
            capacity * PayloadStride, true };
    }

    // Consumer must also verify frame stamp, resource lifetime, and fence.
    constexpr bool ValidPublicationLayout(uint32_t version, uint32_t width, uint32_t height,
        uint64_t pitch, uint64_t capacity, uint64_t directoryBytes, uint64_t payloadBytes)
    {
        const auto layout = Plan(width, height);
        return version == Version && layout.valid && pitch == layout.pitch &&
            capacity <= layout.capacity && directoryBytes >= layout.directoryBytes &&
            payloadBytes >= capacity * PayloadStride;
    }
    constexpr Directory Sanitize(Directory d, uint32_t capacity)
    {
        return Publish(d.present, d.valid, d.ordinal, capacity);
    }
}

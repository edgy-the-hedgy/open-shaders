#pragma once
#include <cstdint>

namespace NativeIBL::Policy
{
    // Zero is reserved for absent indirect publications. Include optional
    // source revisions even when the filtered base generation is unchanged.
    constexpr uint64_t EnvironmentContentKey(uint64_t epoch, uint64_t generation,
        uint32_t encoding, uint64_t base, uint64_t reflection, uint64_t sky)
    {
        if (!epoch || !generation || !base) return 0;
        uint64_t content = 14695981039346656037ull;
        const auto mix = [&](uint64_t value) { content ^= value; content *= 1099511628211ull; };
        mix(epoch); mix(generation); mix(encoding); mix(base); mix(reflection); mix(sky);
        return content ? content : 1u;
    }
}

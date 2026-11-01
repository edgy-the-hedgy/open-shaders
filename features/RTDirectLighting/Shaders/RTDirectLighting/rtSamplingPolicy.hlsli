#ifndef RT_SAMPLING_POLICY_HLSLI
#define RT_SAMPLING_POLICY_HLSLI

// Matching GPU implementation of DX12Native::RTSampling.
// The caller supplies a phase derived from a monotonic 64-bit CPU sequence.
// Full mode deliberately bypasses all hashing and temporal variation.
static const uint RTSampleFull = 0u;
static const uint RTSampleThreeQuarters = 1u;
static const uint RTSampleTwoThirds = 2u;
static const uint RTSampleCheckerboard = 3u;
static const uint RTSampleFourNinths = 4u;
static const uint RTSampleQuarter = 5u;
static const uint RTSampleNinth = 6u;
static const uint RTSampleSixteenth = 7u;

uint RTSample_Period(uint mode)
{
    if (mode == RTSampleThreeQuarters || mode == RTSampleQuarter) return 4u;
    if (mode == RTSampleTwoThirds) return 3u;
    if (mode == RTSampleCheckerboard) return 2u;
    if (mode == RTSampleFourNinths || mode == RTSampleNinth) return 9u;
    if (mode == RTSampleSixteenth) return 16u;
    return 1u;
}

uint2 RTSample_TileSize(uint mode)
{
    if (mode == RTSampleThreeQuarters || mode == RTSampleCheckerboard || mode == RTSampleQuarter)
        return uint2(2u, 2u);
    if (mode == RTSampleTwoThirds) return uint2(3u, 1u);
    if (mode == RTSampleFourNinths || mode == RTSampleNinth) return uint2(3u, 3u);
    if (mode == RTSampleSixteenth) return uint2(4u, 4u);
    return uint2(1u, 1u);
}

uint RTSample_Hash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

// signalKind is intentionally absent: equal-density direct and GI masks
// share phase/rank, allowing the scheduler to reuse the same primary query.
bool RTSample_IsSampled(uint2 pixel, uint phase, uint mode)
{
    if (mode == RTSampleFull || mode > RTSampleSixteenth) return true;
    phase %= RTSample_Period(mode);
    if (mode == RTSampleCheckerboard)
        return ((pixel.x + pixel.y + phase) & 1u) == 0u;

    const uint2 tileSize = RTSample_TileSize(mode);
    const uint2 tile = pixel / tileSize;
    const uint2 local = pixel % tileSize;
    const uint seed = RTSample_Hash(tile.x * 0x9e3779b9u ^ tile.y * 0x85ebca6bu);

    if (mode == RTSampleThreeQuarters || mode == RTSampleQuarter) {
        const uint index = local.y == 0u ? local.x : 3u - local.x;
        const uint rank = (index + 2u * (seed & 1u) + phase) & 3u;
        return rank < (mode == RTSampleQuarter ? 1u : 3u);
    }
    if (mode == RTSampleTwoThirds)
        return ((local.x + seed % 3u + phase) % 3u) < 2u;
    if (mode == RTSampleFourNinths || mode == RTSampleNinth) {
        const uint rank = (5u * (local.y * 3u + local.x) + seed % 9u + phase) % 9u;
        return rank < (mode == RTSampleNinth ? 1u : 4u);
    }
    return ((5u * (local.y * 4u + local.x) + seed % 16u + phase) & 15u) == 0u;
}

struct RTSample_Anchor
{
    uint2 pixel;
    uint valid;
};

RTSample_Anchor RTSample_GetTraceAnchor(uint2 pixel, uint2 size, uint phase, uint mode)
{
    RTSample_Anchor anchor;
    anchor.pixel = pixel;
    anchor.valid = 0u;
    if (any(pixel >= size)) return anchor;
    if (RTSample_IsSampled(pixel, phase, mode)) {
        anchor.valid = 1u;
        return anchor;
    }
    const uint2 tileSize = RTSample_TileSize(mode);
    const uint2 origin = (pixel / tileSize) * tileSize;
    const uint2 limits = min(tileSize, size - origin);
    uint bestDistance = 0xffffffffu;
    [loop] for (uint y = 0u; y < limits.y; ++y) {
        [loop] for (uint x = 0u; x < limits.x; ++x) {
            const uint2 candidate = origin + uint2(x, y);
            if (!RTSample_IsSampled(candidate, phase, mode)) continue;
            const int2 delta = int2(candidate) - int2(pixel);
            const uint distance = uint(dot(delta, delta));
            if (distance < bestDistance) {
                bestDistance = distance;
                anchor.pixel = candidate;
                anchor.valid = 1u;
            }
        }
    }
    // Uncovered incomplete tiles must use raster fallback, not stale UAV data.
    return anchor;
}

#endif

#ifndef FULL_NATIVE_SAMPLING_HLSLI
#define FULL_NATIVE_SAMPLING_HLSLI
#include "Common/NativeShadowRT.hlsli"

// Same logical grid and checkerboard parity as RT Shadows. Invalid packed
// lanes return an out-of-range sentinel, never a clamped duplicate receiver.
uint2 NativeSamplePixel(uint2 packed, uint2 size, uint quality, uint frame)
{
    uint2 logicalSize = uint2(ShadowLogicalExtent(size.x, quality), ShadowLogicalExtent(size.y, quality));
    uint2 logical = uint2(ShadowLogicalX(packed.x, packed.y, frame & 1u), packed.y);
    if (any(logical >= logicalSize)) return size;
    return uint2(ShadowRenderCoordinate(logical.x, size.x, quality), ShadowRenderCoordinate(logical.y, size.y, quality));
}
bool NativeIsSampled(uint2 pixel, uint2 size, uint quality, uint frame)
{
    return ShadowIsSampled(pixel.x, pixel.y, size.x, size.y, quality, frame);
}
uint NativeEmitterHash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}
float NativeEmitterRandom(uint seed, uint salt)
{
    return (float(NativeEmitterHash(seed ^ salt) >> 9u) + 0.5) / 8388608.0;
}
float3 NativeEmitterOffset(float3 direction, float radius, uint2 pixel, uint light, uint frame)
{
    if (radius <= 0.0) return 0.0;
    const uint seed = NativeEmitterHash(pixel.x * 0x9e3779b9u ^ pixel.y * 0x85ebca6bu ^ frame * 0xc2b2ae35u ^ light * 0x27d4eb2fu);
    float radial = radius * sqrt(NativeEmitterRandom(seed, 0x68bc21ebu));
    float angle = NativeEmitterRandom(seed, 0x02e5be93u) * 6.28318530718;
    float3 tangent = normalize(cross(direction, abs(direction.z) < 0.99 ? float3(0,0,1) : float3(0,1,0)));
    return radial * (cos(angle) * tangent + sin(angle) * cross(direction, tangent));
}
#endif

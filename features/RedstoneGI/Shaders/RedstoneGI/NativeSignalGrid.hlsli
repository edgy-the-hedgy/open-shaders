#ifndef NATIVE_SIGNAL_GRID
#define NATIVE_SIGNAL_GRID
cbuffer NativeSampling : register(b13)
{
    uint2 NativeLogicalSize;
    uint2 NativePackedSize;
    uint NativeCheckerboard;
    uint NativeOrigin;
    uint NativeClampEnabled;
    float NativeClampThreshold;
};
uint2 NativeLogicalPixel(uint2 packed)
{
    return uint2(NativeCheckerboard ? packed.x * 2u + ((packed.y + NativeOrigin) & 1u) : packed.x, packed.y);
}
float4 ValidateNativeRadiance(float4 signal)
{
    if (!all(isfinite(signal))) return float4(0, 0, 0, -2);
    signal.rgb = max(signal.rgb, 0);
    if (NativeClampEnabled) signal.rgb = min(signal.rgb, NativeClampThreshold);
    return float4(min(signal.rgb, 65504.0), clamp(signal.a, -2.0, 65504.0));
}
#endif

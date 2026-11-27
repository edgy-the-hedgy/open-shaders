#ifndef NATIVE_ENVIRONMENT_COLOR_HLSLI
#define NATIVE_ENVIRONMENT_COLOR_HLSLI
#include "Common/ColorSpaces.hlsli"
// Match EnvironmentEncoding; native material radiance is linear Rec.709.
float3 DecodeEnvironmentRadiance(float3 color, uint encoding)
{
    if (encoding == 0u) return pow(abs(color), 1.6);
    if (encoding == 2u) return AP1TosRGB(color);
    return color;
}
#endif

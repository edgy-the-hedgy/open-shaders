#ifndef NATIVE_INPUT_COVERAGE_HLSLI
#define NATIVE_INPUT_COVERAGE_HLSLI
// Preserve the engine's 4x4 fade mask and raw UV coverage test. No material lighting.
namespace NativeInputCoverage
{
    float Alpha(float sampled, float material, float vertex, float2 pixel, bool dither)
    {
        if (dither) {
            const float maskValues[16] = {0.003922,0.533333,0.133333,0.666667,0.800000,0.266667,0.933333,0.400000,
                0.200000,0.733333,0.066667,0.600000,0.996078,0.466667,0.866667,0.333333};
            uint2 p = uint2(pixel);
            clip(material - maskValues[((p.x << 2) & 12u) | (p.y & 3u)]);
            return sampled * vertex;
        }
        return sampled * material * vertex;
    }
}
#endif

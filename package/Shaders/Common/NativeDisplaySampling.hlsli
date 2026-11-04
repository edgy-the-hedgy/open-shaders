#ifndef NATIVE_DISPLAY_SAMPLING
#define NATIVE_DISPLAY_SAMPLING
#ifdef __cplusplus
#include <cstdint>
#include <cmath>
namespace DX12Native {
using uint = uint32_t;
#define DISPLAY_CONSTEXPR constexpr
#else
#define DISPLAY_CONSTEXPR
#endif
DISPLAY_CONSTEXPR uint NativeDisplayRadius(uint traceMode)
{
    return traceMode == 0u ? 0u : traceMode == 2u ? 4u : traceMode == 1u ? 2u : 3u;
}
DISPLAY_CONSTEXPR uint NativeDisplayLogical(uint pixel, uint nativeSize, uint logicalSize)
{
    return pixel * logicalSize / nativeSize;
}
DISPLAY_CONSTEXPR uint NativeDisplayReceiver(uint logical, uint nativeSize, uint logicalSize)
{
    return ((logical * 2u + 1u) * nativeSize) / (logicalSize * 2u);
}
DISPLAY_CONSTEXPR bool NativeDisplayPrefer(float distance, float bestDistance, bool valid)
{
    return valid && distance < bestDistance;
}
DISPLAY_CONSTEXPR bool NativeDisplaySurface(float receiverIdentity, float sampleIdentity,
    float distanceSquared, float receiverDistanceSquared)
{
    const float minimumDistanceSquared = 0.25f;
    const float relativeDistanceSquared = receiverDistanceSquared * 0.000025f;
    return receiverIdentity != 0 && receiverIdentity == sampleIdentity && distanceSquared >= 0 &&
        distanceSquared <= (relativeDistanceSquared > minimumDistanceSquared ? relativeDistanceSquared : minimumDistanceSquared);
}
DISPLAY_CONSTEXPR bool NativeDisplayUseRR(uint recordedMask, uint signal, bool outputFinite, bool surfaceCompatible)
{
    return (recordedMask & (1u << signal)) != 0 && outputFinite && surfaceCompatible;
}
#ifdef __cplusplus
}
#endif
#undef DISPLAY_CONSTEXPR
#endif

#ifndef NATIVE_SHADOW_MASK_CONTRACT
#define NATIVE_SHADOW_MASK_CONTRACT
#ifdef __cplusplus
#include <cstdint>
namespace DX12Native {
using uint = uint32_t;
#define SHADOW_CONSTEXPR constexpr
#else
#define SHADOW_CONSTEXPR
#endif
static const uint NativeShadowTracePass = 4u;
SHADOW_CONSTEXPR float NativeShadowClip(uint pixel, uint size)
{
    return (float(pixel) + 0.5f) * 2.0f / float(size) - 1.0f;
}
SHADOW_CONSTEXPR bool NativeShadowValid(bool receiver, bool geometry, float distance)
{
    return receiver && geometry && distance > 0.0f && distance <= 60000.0f;
}
SHADOW_CONSTEXPR float NativeShadowVisibility(bool valid, bool blocked)
{
    return valid && blocked ? 0.0f : 1.0f;
}
SHADOW_CONSTEXPR float NativeShadowFactor(float visibility, float strength, float blend)
{
    return 1.0f - (1.0f - visibility) * strength * blend;
}
SHADOW_CONSTEXPR bool NativeShadowReceiverMatches(float errorSquared, float tolerance)
{
    return errorSquared >= 0.0f && errorSquared <= tolerance * tolerance;
}
#ifdef __cplusplus
}
#endif
#undef SHADOW_CONSTEXPR
#endif

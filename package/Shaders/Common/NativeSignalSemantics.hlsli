#ifndef NATIVE_SIGNAL_SEMANTICS
#define NATIVE_SIGNAL_SEMANTICS
#ifdef __cplusplus
namespace DX12Native {
#define SIGNAL_CONSTEXPR constexpr
#else
#define SIGNAL_CONSTEXPR
#endif
SIGNAL_CONSTEXPR float NativeDirectAlpha(bool active)
{
    return active ? 0.0f : -1.0f;
}
SIGNAL_CONSTEXPR bool NativeRRChannelValid(float value)
{
    return value >= 0.0f && value <= 65504.0f;
}
#ifdef __cplusplus
}
#endif
#undef SIGNAL_CONSTEXPR
#endif

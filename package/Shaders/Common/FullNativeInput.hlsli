#ifndef FULL_NATIVE_INPUT_CONTRACT_HLSLI
#define FULL_NATIVE_INPUT_CONTRACT_HLSLI

// Full Native input ABI is selected once at boot. Deferred Lighting is
// visibility-only even when a native material or MRT publication is unavailable.
#if defined(FULL_NATIVE_INPUT)
#   if defined(FULL_NATIVE_LIGHTING_SHADER)
#       define NATIVE_PBR_INPUT
#   endif
#   define NATIVE_IBL_INPUT
#endif

#endif

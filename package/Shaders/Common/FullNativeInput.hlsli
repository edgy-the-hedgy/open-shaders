#ifndef FULL_NATIVE_INPUT_CONTRACT_HLSLI
#define FULL_NATIVE_INPUT_CONTRACT_HLSLI

// Full Native input ABI is selected once at boot. The raster color path is
// retained for unsupported receivers until ownership coverage is certified.
#if defined(FULL_NATIVE_INPUT)
#   if defined(FULL_NATIVE_LIGHTING_SHADER)
#       define NATIVE_PBR_INPUT
#   endif
#   if !defined(FULL_NATIVE_LEGACY_ENVIRONMENT)
#       define NATIVE_IBL_INPUT
#   endif
#endif

#endif

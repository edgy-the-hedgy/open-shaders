#ifndef NATIVE_SURFACE_HLSLI
#define NATIVE_SURFACE_HLSLI
static const uint NativeSurfaceStride = 32u;
static const float SurfaceInvalidReceiver = 0.0;
static const float SurfaceEvaluatedBase = 1.0;
static const float SurfaceFeatureLimited = 2.0;
static const float SurfaceMissingGeometry = -1.0;
static const float SurfaceDepthMismatch = -2.0;
static const float SurfaceEvaluationFailed = -3.0;
static const float SurfaceUnsupportedMaterial = -4.0;
static const float SurfaceMissingTextures = -5.0;
static const uint SurfaceBaseOnlyBinding = 1u << 11;
static const uint SurfaceMissingBaseTexturesBinding = 1u << 12;

bool HasEvaluatedNativeBase(float status)
{
    return status == SurfaceEvaluatedBase || status == SurfaceFeatureLimited;
}
#endif

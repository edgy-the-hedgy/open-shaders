#ifndef NATIVE_SURFACE_HLSLI
#define NATIVE_SURFACE_HLSLI
static const uint NativeSurfaceStride = 32u;
static const uint NativeOpaqueRadianceStride = 8u;
static const uint ContributionDirectDiffuse = 1u << 0;
static const uint ContributionDirectSpecular = 1u << 1;
static const uint ContributionIndirectDiffuse = 1u << 2;
static const uint ContributionIndirectSpecular = 1u << 3;
static const uint ContributionTransmission = 1u << 4;
static const uint ContributionEmission = 1u << 5;
static const uint ContributionRemainingOpaque = 1u << 6;
static const uint ContributionIndependentVisibility = 1u << 7;
static const uint NativeOpaqueRequired = 127u;
static const uint RasterIdentityCandidate = 1u << 0;
static const uint ExactRasterWinner = 1u << 1;
static const uint CompleteOpaqueRadiance = 1u << 2;
static const uint NativeOwnsOpaque = 1u << 3;
static const uint NativeOpaqueOwnershipRequired = 15u;
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

bool HasNativeOpaqueOwnership(uint ownership)
{
    return ownership == NativeOpaqueOwnershipRequired;
}

bool HasEvaluatedNativeBase(float status)
{
    return status == SurfaceEvaluatedBase || status == SurfaceFeatureLimited;
}
// A producer writes zero validity on overflow or invalid radiance, never a fabricated valid black pixel.
uint2 PackOpaqueRadiance(float3 linearColor, uint valid)
{
    if (!all(isfinite(linearColor)) || any(abs(linearColor) > 65504.0) || (valid & ~255u) != 0u)
        return uint2(0,0);
    uint4 halfBits = f32tof16(float4(linearColor, float(valid)));
    return uint2(halfBits.x | (halfBits.y << 16), halfBits.z | (halfBits.w << 16));
}

bool OpaqueRadianceValid(float status, float4 sample, uint available, uint required)
{
    if (status != SurfaceEvaluatedBase || !all(isfinite(sample)) ||
        sample.a < 0.0 || sample.a > 255.0 || sample.a != floor(sample.a) || required != NativeOpaqueRequired)
        return false;
    uint valid = uint(sample.a);
    return (valid & ~available) == 0u && (valid & required) == required;
}
#endif

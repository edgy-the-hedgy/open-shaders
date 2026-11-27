#ifndef NATIVE_SURFACE_HLSLI
#define NATIVE_SURFACE_HLSLI
static const uint NativeSurfaceStride = 32u;
static const uint NativeLightingStride = 24u;
static const uint NativeMaterialLobeStride = 32u;
static const uint SurfaceCoatReadyBinding = 1u << 13;
static const uint SurfaceSubsurfaceReadyBinding = 1u << 14;
struct NativeCoatSample { float4 normalRoughness; float4 colorStrength; float4 f0Colored; float4 subsurface; };
float4 UnpackNativeLobeHalf4(uint2 v) { return f16tof32(uint4(v.x & 65535u, v.x >> 16, v.y & 65535u, v.y >> 16)); }
NativeCoatSample ReadNativeCoat(ByteAddressBuffer lobes, uint offset)
{
    NativeCoatSample coat;
    coat.normalRoughness = UnpackNativeLobeHalf4(lobes.Load2(offset));
    coat.colorStrength = UnpackNativeLobeHalf4(lobes.Load2(offset + 8u));
    coat.f0Colored = UnpackNativeLobeHalf4(lobes.Load2(offset + 16u));
    coat.subsurface = UnpackNativeLobeHalf4(lobes.Load2(offset + 24u));
    return coat;
}
bool HasNativeCoat(NativeCoatSample coat)
{
    return coat.colorStrength.w > 0.0 && all(isfinite(coat.normalRoughness)) && all(isfinite(coat.colorStrength)) &&
        all(isfinite(coat.f0Colored)) && dot(coat.normalRoughness.xyz, coat.normalRoughness.xyz) > 1e-8;
}
bool HasNativeSubsurface(NativeCoatSample material)
{
    return all(isfinite(material.subsurface)) && any(material.subsurface.rgb > 0.0) && material.subsurface.w < 1.0;
}
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

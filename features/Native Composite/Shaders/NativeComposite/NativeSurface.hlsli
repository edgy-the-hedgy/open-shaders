#ifndef NATIVE_SURFACE_HLSLI
#define NATIVE_SURFACE_HLSLI
static const uint NativeSurfaceStride = 32u;
static const uint NativeLightingStride = 24u;
static const uint NativeMaterialLobeStride = 32u;
// Optional tail plane after the fixed lobe plane. Base and lobe ABIs are unchanged.
static const uint NativeParallaxShadowStride = 32u;
struct NativeParallaxShadowSample {
    float2 uv;
    uint textureAndMode;
    float height;
    float4 tangentScale;
    float4 bitangentMip;
};
NativeParallaxShadowSample ReadNativeParallaxShadow(ByteAddressBuffer lobes, uint offset)
{
    NativeParallaxShadowSample sample;
    uint4 header=lobes.Load4(offset);
    sample.uv=asfloat(header.xy);
    sample.textureAndMode=header.z;
    sample.height=asfloat(header.w);
    uint4 basis=lobes.Load4(offset+16u);
    sample.tangentScale=f16tof32(uint4(basis.x&65535u,basis.x>>16,basis.y&65535u,basis.y>>16));
    sample.bitangentMip=f16tof32(uint4(basis.z&65535u,basis.z>>16,basis.w&65535u,basis.w>>16));
    return sample;
}
static const uint SurfaceCoatReadyBinding = 1u << 13;
static const uint SurfaceSubsurfaceReadyBinding = 1u << 14;
static const uint SurfaceThinSurfaceReadyBinding = 1u << 15;
static const uint SurfaceParallaxReadyBinding = 1u << 4;
static const uint SurfaceFuzzReadyBinding = 1u << 5;
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
    return coat.f0Colored.w>=0.0 && coat.colorStrength.w > 0.0 && all(isfinite(coat.normalRoughness)) && all(isfinite(coat.colorStrength)) &&
        all(isfinite(coat.f0Colored)) && dot(coat.normalRoughness.xyz, coat.normalRoughness.xyz) > 1e-8;
}
// Standalone fuzz uses the mutually exclusive coat payload. A zero normal
// keeps it out of HasNativeCoat; the tag distinguishes it from thin surfaces.
static const float NativeThinTag = -1.0;
static const float NativeFuzzTag = -2.0;
// Thin sheets need normalRoughness.w for scattering balance. Encode fuzz
// roughness in the tag instead: [-4,-3] explicit, -5 legacy substrate mode.
static const float NativeThinFuzzTag = -3.0;
static const float NativeThinFuzzLegacyTag = -5.0;
bool IsNativeThinFuzzTag(float tag)
{
    return (tag>=NativeThinFuzzTag-1.0 && tag<=NativeThinFuzzTag) || tag==NativeThinFuzzLegacyTag;
}
float EncodeNativeThinFuzz(float roughness)
{
    return roughness<0.0 ? NativeThinFuzzLegacyTag : NativeThinFuzzTag-saturate(roughness);
}
float NativeFuzzRoughness(NativeCoatSample material)
{
    float tag=material.f0Colored.w;
    if (tag==NativeThinFuzzLegacyTag) return -1.0;
    return IsNativeThinFuzzTag(tag) ? NativeThinFuzzTag-tag : material.normalRoughness.w;
}
bool HasNativeFuzz(NativeCoatSample material)
{
    return (material.f0Colored.w==NativeFuzzTag || IsNativeThinFuzzTag(material.f0Colored.w)) &&
        all(isfinite(material.colorStrength)) && isfinite(NativeFuzzRoughness(material)) &&
        material.colorStrength.w>0.0;
}
// Interlayer refraction is mutually exclusive with subsurface scattering.
// The unused scattering W component of a coat record stores 2 + eta.
bool HasNativeInterlayer(NativeCoatSample coat)
{
    return HasNativeCoat(coat) && coat.subsurface.w>=2.0 && coat.subsurface.w<=3.0 &&
        all(coat.subsurface.xyz==0.0);
}
float3 NativeRefractDirection(float3 direction, float3 normal, float eta)
{
    if (dot(direction,normal)<=0.0) return direction;
    float3 candidate=-refract(-direction,normal,eta);
    return all(isfinite(candidate)) && dot(candidate,candidate)>1e-8 ? normalize(candidate) : direction;
}
float3 NativeBaseDirection(float3 direction, NativeCoatSample coat)
{
    return HasNativeInterlayer(coat) ? NativeRefractDirection(direction,normalize(coat.normalRoughness.xyz),
        saturate(coat.subsurface.w-2.0)) : direction;
}
// Coat and thin-surface modes are mutually exclusive in the authored material guide.
// The thin variant reuses the coat fields for face normal, balance and specular controls.
bool HasNativeThinSurface(NativeCoatSample material)
{
    return (material.f0Colored.w==NativeThinTag || IsNativeThinFuzzTag(material.f0Colored.w)) && all(isfinite(material.normalRoughness)) &&
        all(isfinite(material.f0Colored)) && all(isfinite(material.subsurface)) &&
        dot(material.normalRoughness.xyz,material.normalRoughness.xyz)>1e-8;
}
void NativeThinAlbedos(float3 base, NativeCoatSample material, out float3 reflection, out float3 front, out float3 back)
{
    float opacity=saturate(material.subsurface.w);
    float share=1.0-opacity;
    float balance=saturate(material.normalRoughness.w);
    reflection=saturate(base)*opacity;
    front=saturate(material.subsurface.rgb*share*(1.0-balance));
    back=saturate(material.subsurface.rgb*share*balance);
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

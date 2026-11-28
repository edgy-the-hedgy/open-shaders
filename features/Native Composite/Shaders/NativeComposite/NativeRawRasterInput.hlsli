#ifndef NATIVE_RAW_RASTER_INPUT_HLSLI
#define NATIVE_RAW_RASTER_INPUT_HLSLI
// ABI shared with the standalone ps_5_0 input capture. No raster radiance.
#include "NativeRawRasterSurface.hlsli"
Texture2D<uint4> RawRasterIdentityUV : register(t1031);
Texture2D<uint4> RawRasterColorGradDepth : register(t1032);
Texture2D<uint4> RawRasterBasisFlags : register(t1033);
Texture2D<uint4> RawRasterMaterialTechnique : register(t1034);
Texture2D<float4> RawRasterParticipation : register(t1035);
float2 RawHalf2(uint v) { return f16tof32(uint2(v & 65535u, v >> 16)); }
float3 RawOct(uint v)
{
    float2 p = float2(v & 65535u, v >> 16) / 65535.0 * 2.0 - 1.0;
    float3 n = float3(p, 1.0 - abs(p.x) - abs(p.y));
    if (n.z < 0) {
        float2 signP = float2(p.x >= 0 ? 1.0 : -1.0, p.y >= 0 ? 1.0 : -1.0);
        n.xy = (1.0 - abs(p.yx)) * signP;
    }
    return normalize(n);
}
RawRasterSurface ReadRawRaster(uint2 pixel, float depth, uint instance, uint primitive, uint hasRawInput)
{
    RawRasterSurface result = (RawRasterSurface)0;
    if (!hasRawInput) return result;
    uint4 a = RawRasterIdentityUV.Load(int3(pixel,0));
    if (!a.x || a.x-1u != instance || a.y != primitive) return result;
    uint4 c = RawRasterBasisFlags.Load(int3(pixel,0));
    if ((c.w & 6u) != 6u) return result;
    // Participation is not identity. A later nonparticipating writer can
    // invalidate the witness; exact correspondence still requires the ray hit.
    if (RawRasterParticipation.Load(int3(pixel,0)).z != 1.0) return result;
    uint4 b = RawRasterColorGradDepth.Load(int3(pixel,0));
    float rawDepth = asfloat(b.w);
    if (!isfinite(rawDepth) || abs(rawDepth-depth)>1e-6) return result;
    result.uv=asfloat(a.zw);
    result.dx=RawHalf2(b.y); result.dy=RawHalf2(b.z);
    result.color=float4(b.x&255u,(b.x>>8)&255u,(b.x>>16)&255u,b.x>>24)/255.0;
    result.tangent=RawOct(c.x); result.bitangent=RawOct(c.y); result.normal=RawOct(c.z);
    uint4 material = RawRasterMaterialTechnique.Load(int3(pixel,0));
    result.materialIndex=material.x; result.pixelDescriptor=material.y;
    result.extraDescriptor=material.z; result.extraFeatureDescriptor=material.w;
    result.alpha=float(c.w >> 3u) / 65535.0;
    result.covered=true;
    result.front=(c.w&1u)!=0u;
    result.valid=all(isfinite(result.uv)) && all(isfinite(result.dx)) && all(isfinite(result.dy)) &&
        all(isfinite(result.tangent)) && all(isfinite(result.bitangent)) && all(isfinite(result.normal));
    return result;
}
bool ReadRawRasterCandidate(uint2 pixel, float depth, uint hasRawInput, out uint2 identity, out RawRasterSurface raw)
{
    identity = uint2(0xffffffffu, 0xffffffffu);
    raw = (RawRasterSurface)0;
    if (!hasRawInput) return false;
    uint4 a = RawRasterIdentityUV.Load(int3(pixel,0));
    if (!a.x || (a.y & 0x80000000u) != 0u) return false;
    identity = uint2(a.x - 1u, a.y);
    raw = ReadRawRaster(pixel, depth, identity.x, identity.y, hasRawInput);
    return raw.valid;
}
#endif

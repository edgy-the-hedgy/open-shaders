#ifndef NATIVE_RAW_RASTER_INPUT_HLSLI
#define NATIVE_RAW_RASTER_INPUT_HLSLI
// ABI v6 shared with the original opaque-pass FXC shaders. No raster radiance.
#include "NativeRawRasterSurface.hlsli"
#include "NativeRasterDrawIdentity.hlsli"
Texture2D<uint4> RawRasterIdentityUV : register(t1031);
Texture2D<uint4> RawRasterColorGradDepth : register(t1032);
Texture2D<uint4> RawRasterBasisFlags : register(t1033);
Texture2D<uint4> RawRasterMaterialTechnique : register(t1034);
Texture2D<float4> RawRasterParticipation : register(t1035);
ByteAddressBuffer RawRasterDrawTable : register(t1036);
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
RawRasterSurface ReadRawVisibility(uint2 pixel, float depth, uint drawCount, out uint2 sceneIdentity)
{
    RawRasterSurface result = (RawRasterSurface)0;
    sceneIdentity = uint2(0xffffffffu, 0xffffffffu);
    if (!drawCount) return result;
    uint4 a = RawRasterIdentityUV.Load(int3(pixel,0));
    uint address;
    if (!DecodeRasterDraw(RawRasterDrawTable, drawCount, a.xy, address, sceneIdentity)) return result;
    result.drawIdentity=a.xy;
    uint4 drawIdentity=RawRasterDrawTable.Load4(address);
    result.geometryOrdinal=drawIdentity.y;
    result.materialOrdinal=drawIdentity.z;
    uint4 c = RawRasterBasisFlags.Load(int3(pixel,0));
    if ((c.w & 6u) != 6u) return result;
    // The pass certificate is supplied as a nonzero draw count. The witness
    // and final depth must agree with the integer visibility write.
    if (RawRasterParticipation.Load(int3(pixel,0)).z != 1.0) return result;
    uint4 b = RawRasterColorGradDepth.Load(int3(pixel,0));
    float rawDepth = asfloat(b.w);
    if (!isfinite(rawDepth) || !isfinite(depth) || abs(rawDepth-depth)>1e-6) return result;
    result.uv=asfloat(a.zw);
    result.dx=RawHalf2(b.y); result.dy=RawHalf2(b.z);
    result.color=float4(b.x&255u,(b.x>>8)&255u,(b.x>>16)&255u,b.x>>24)/255.0;
    result.tangent=RawOct(c.x); result.bitangent=RawOct(c.y); result.normal=RawOct(c.z);
    uint4 material = RawRasterMaterialTechnique.Load(int3(pixel,0));
    result.auxiliaryKind=(c.w >> 19u)&15u;
    result.borrowedLandscape=(c.w & (1u<<23u))!=0u;
    result.packedSnow=result.auxiliaryKind==1u ? float(c.w>>24u)/255.0 : 0.0;
    result.auxiliary=material;
    uint4 drawRange=RawRasterDrawTable.Load4(address+16u);
    uint4 descriptors=RawRasterDrawTable.Load4(address+48u);
    result.materialIndex=all(sceneIdentity!=0xffffffffu) ? drawRange.z : 0xffffffffu;
    result.pixelDescriptor=descriptors.x;
    result.vertexDescriptor=descriptors.y;
    result.extraDescriptor=descriptors.z;
    result.extraFeatureDescriptor=descriptors.w;
    result.alpha=float((c.w >> 3u)&65535u) / 65535.0;
    result.covered=true;
    result.front=(c.w&1u)!=0u;
    result.valid=all(isfinite(result.uv)) && all(isfinite(result.dx)) && all(isfinite(result.dy)) &&
        all(isfinite(result.tangent)) && all(isfinite(result.bitangent)) && all(isfinite(result.normal));
    return result;
}
RawRasterSurface ReadRawRaster(uint2 pixel, float depth, uint instance, uint primitive, uint drawCount)
{
    uint2 mapped;
    RawRasterSurface raw=ReadRawVisibility(pixel,depth,drawCount,mapped);
    if (any(mapped!=uint2(instance,primitive))) raw.valid=false;
    return raw;
}
bool ReadRawRasterCandidate(uint2 pixel, float depth, uint drawCount, out uint2 identity, out RawRasterSurface raw)
{
    raw=ReadRawVisibility(pixel,depth,drawCount,identity);
    return raw.valid && all(identity!=0xffffffffu);
}
#endif

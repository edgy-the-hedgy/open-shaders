#ifndef NATIVE_RAW_RASTER_INPUT_HLSLI
#define NATIVE_RAW_RASTER_INPUT_HLSLI
// ABI shared with the standalone ps_5_0 input capture. No raster radiance.
struct RawRasterSurface
{
    float2 uv, dx, dy;
    float4 color;
    float3 tangent, bitangent, normal;
    bool front, valid;
};
Texture2DArray<uint4> RawRasterPlanes : register(t1031);
Texture2D<float4> RawRasterFinalTokens : register(t1032);
ByteAddressBuffer RawRasterTokenTable : register(t1033);
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
RawRasterSurface ReadRawRaster(uint2 pixel, float depth, uint instance, uint primitive, uint tokenCount)
{
    RawRasterSurface result = (RawRasterSurface)0;
    if (!tokenCount) return result;
    uint4 a = RawRasterPlanes.Load(int4(pixel,0,0));
    if (!a.x || a.x-1u != instance || a.y != primitive) return result;
    uint4 c = RawRasterPlanes.Load(int4(pixel,2,0));
    uint token = c.w & 65535u;
    if ((c.w & 131072u) == 0u || !token || token > tokenCount) return result;
    // The final blended G-buffer token is only a corroborating candidate.
    // Require its table entry and the independently intersected primitive.
    float finalToken = RawRasterFinalTokens.Load(int3(pixel,0)).z;
    if (!isfinite(finalToken) || uint(round(saturate(finalToken)*65535.0)) != token ||
        any(RawRasterTokenTable.Load2((token-1u)*8u) != uint2(instance,primitive))) return result;
    uint4 b = RawRasterPlanes.Load(int4(pixel,1,0));
    float rawDepth = asfloat(b.w);
    if (!isfinite(rawDepth) || abs(rawDepth-depth)>1e-6) return result;
    result.uv=asfloat(a.zw);
    result.dx=RawHalf2(b.y); result.dy=RawHalf2(b.z);
    result.color=float4(b.x&255u,(b.x>>8)&255u,(b.x>>16)&255u,b.x>>24)/255.0;
    result.tangent=RawOct(c.x); result.bitangent=RawOct(c.y); result.normal=RawOct(c.z);
    result.front=(c.w&65536u)!=0u;
    result.valid=all(isfinite(result.uv)) && all(isfinite(result.dx)) && all(isfinite(result.dy)) &&
        all(isfinite(result.tangent)) && all(isfinite(result.bitangent)) && all(isfinite(result.normal));
    return result;
}
#endif

#ifndef NATIVE_RAW_RASTER_SURFACE_HLSLI
#define NATIVE_RAW_RASTER_SURFACE_HLSLI
struct RawRasterSurface
{
    float2 uv, dx, dy;
    float4 color;
    float3 tangent, bitangent, normal;
    bool front, valid;
};
#endif

#ifndef NATIVE_RAW_RASTER_SURFACE_HLSLI
#define NATIVE_RAW_RASTER_SURFACE_HLSLI
struct RawRasterSurface
{
    float2 uv, dx, dy;
    float4 color;
    float3 tangent, bitangent, normal;
    uint materialIndex, pixelDescriptor, extraDescriptor, extraFeatureDescriptor;
    float alpha;
    uint2 drawIdentity;
    uint geometryOrdinal, materialOrdinal;
    uint auxiliaryKind;
    uint4 auxiliary;
    bool covered, front, valid;
};
#endif

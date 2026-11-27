#ifndef NATIVE_MATERIAL_ALPHA_HLSLI
#define NATIVE_MATERIAL_ALPHA_HLSLI
struct MaterialRecord {
    uint4 textures[4]; float4 uv[2]; float4 base; float4 coat; float4 surface;
    float4 projected; float4 coatColor; float4 subsurfaceColor; float4 fuzzColor;
    float4 thinSurface; float4 glint; float4 projectedGlint;
    uint4 properties; uint4 identity; uint4 controls;
};
struct Binding { uint2 identity; uint2 revision; uint materialIndex; uint flags; };
ByteAddressBuffer Geometry : register(NATIVE_GEOMETRY_REGISTER);
StructuredBuffer<MaterialRecord> MaterialRecords : register(NATIVE_RECORD_REGISTER);
StructuredBuffer<Binding> Bindings : register(NATIVE_BINDING_REGISTER);
Texture2DArray<float4> Textures[512] : register(NATIVE_TEXTURE_REGISTER);
SamplerState ClampClamp : register(s0);
SamplerState ClampWrap : register(s1);
SamplerState WrapClamp : register(s2);
SamplerState WrapWrap : register(s3);
float4 Tex(uint index, float2 uv, uint mode)
{
    uint slot = NonUniformResourceIndex(index);
    if (mode == 0u) return Textures[slot].SampleLevel(ClampClamp, float3(uv,0), 0);
    if (mode == 1u) return Textures[slot].SampleLevel(ClampWrap, float3(uv,0), 0);
    if (mode == 2u) return Textures[slot].SampleLevel(WrapClamp, float3(uv,0), 0);
    return Textures[slot].SampleLevel(WrapWrap, float3(uv,0), 0);
}
bool AcceptAlpha(uint instance,uint primitive,float2 bary, uint instanceCount)
{
    if (instance>=instanceCount) return false;
    Binding binding=Bindings[instance];
    if (binding.materialIndex==0xffffffffu || (binding.flags & 1024u)==0u) return false;
    uint4 offsets=Geometry.Load4(instance*32), counts=Geometry.Load4(instance*32+16);
    if (offsets.z==0xffffffffu || (offsets.w!=36u && offsets.w!=60u) || primitive>=counts.y/3u) return false;
    MaterialRecord material=MaterialRecords[binding.materialIndex];
    if (any(material.identity.xy!=binding.identity) || any(material.identity.zw!=binding.revision) || material.textures[0].x==0xffffffffu) return false;
    uint3 ix=Geometry.Load3(offsets.y+primitive*12u);
    if (any(ix>=counts.x)) return false;
    float3 weights=float3(1.0-bary.x-bary.y,bary);
    float2 uv=0;
    [unroll] for (uint i=0;i<3u;++i)
        uv+=weights[i]*asfloat(Geometry.Load2(offsets.z+ix[i]*offsets.w+12u));
    uv=uv*material.uv[0].zw+material.uv[0].xy;
    float alpha=Tex(material.textures[0].x,uv,material.properties.z).a;
    float threshold=float((binding.flags>>16)&255u)/255.0;
    uint comparison=(binding.flags>>24)&7u;
    return comparison==4u ? alpha>threshold : comparison==6u && alpha>=threshold;
}

#endif

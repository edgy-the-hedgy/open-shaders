#ifndef NATIVE_PROJECTION_HLSLI
#define NATIVE_PROJECTION_HLSLI
#include "Common/Math.hlsli"
#include "Common/Shading.hlsli"
#include "Common/Triplanar.hlsli"
#include "Common/Random.hlsli"

#if defined(NATIVE_SECONDARY_SURFACE)
ByteAddressBuffer ProjectionLookup : register(t529);
Texture2D<uint4> ProjectionRecords : register(t530);
Texture2D<float4> ProjectionTextures[512] : register(t531);
#else
ByteAddressBuffer ProjectionLookup : register(t517);
Texture2D<uint4> ProjectionRecords : register(t518);
Texture2D<float4> ProjectionTextures[512] : register(t519);
#endif
static const uint NativeProjectionTextureCapacity=512u;

float4 NativeProjectionGrad(uint index, uint mode, float2 uv, float2 dx, float2 dy)
{
    uint slot=NonUniformResourceIndex(index);
    if (mode==0u) return ProjectionTextures[slot].SampleGrad(ClampClamp,uv,dx,dy);
    if (mode==1u) return ProjectionTextures[slot].SampleGrad(ClampWrap,uv,dx,dy);
    if (mode==2u) return ProjectionTextures[slot].SampleGrad(WrapClamp,uv,dx,dy);
    return ProjectionTextures[slot].SampleGrad(WrapWrap,uv,dx,dy);
}
float4 NativeProjectionTriplanar(uint index,uint mode,float3 p,float3 dx,float3 dy,float3 weights,float scale,float noise,bool stochastic)
{
    p*=scale; dx*=scale; dy*=scale;
    if (stochastic) {
        if (noise<weights.x) return NativeProjectionGrad(index,mode,p.yz,dx.yz,dy.yz);
        if (noise<weights.x+weights.y) return NativeProjectionGrad(index,mode,p.xz,dx.xz,dy.xz);
        return NativeProjectionGrad(index,mode,p.xy,dx.xy,dy.xy);
    }
    return NativeProjectionGrad(index,mode,p.yz,dx.yz,dy.yz)*weights.x+
        NativeProjectionGrad(index,mode,p.xz,dx.xz,dy.xz)*weights.y+
        NativeProjectionGrad(index,mode,p.xy,dx.xy,dy.xy)*weights.z;
}
// Work in the same linear-sRGB material domain as NativePBRProbe.
// A decoded sRGB view must be re-encoded before applying the authored tint.
float3 NativeProjectionEncode(float3 v)
{
    v=max(v,0.0);
    return lerp(v*12.92,1.055*pow(v,1.0/2.4)-0.055,v>0.0031308);
}
bool NativeApplyProjection(uint row,uint instance,float3 worldPosition,float3 worldDx,float3 worldDy,
    float3x3 objectToWorld,float3x3 worldToObject,float3 face,float3 n,float3 t,float3 b,float vertexAlpha,
    float noise,MaterialRecord material,inout float3 base,inout float3 normal,inout float4 packed,out float projectedWeight,
    float3 rasterDirection,bool useRasterDirection)
{
    projectedWeight=0.0;
    uint4 metadata=ProjectionRecords.Load(int3(0,row,0));
    // Reserved typed material rows are never projector records.
    if ((metadata.x>=0xfffffff9u && metadata.x<=0xfffffffeu) ||
        metadata.z!=row || (!useRasterDirection && metadata.x!=instance)) return false;
    uint4 textures=ProjectionRecords.Load(int3(7,row,0));
    if (textures.w>=NativeProjectionTextureCapacity) return false;
    float3 direction=useRasterDirection ? rasterDirection : mul(objectToWorld,asfloat(ProjectionRecords.Load(int3(3,row,0))).xyz);
    float4 params=asfloat(ProjectionRecords.Load(int3(4,row,0)));
    float4 tint=asfloat(ProjectionRecords.Load(int3(5,row,0)));
    float4 scales=asfloat(ProjectionRecords.Load(int3(6,row,0)));
    if (!all(isfinite(direction)) || dot(direction,direction)<=1e-8 || !all(isfinite(params)) ||
        !all(isfinite(tint)) || !all(isfinite(scales))) return false;
    float3 worldN=normalize(mul(n,worldToObject));
    if (!all(isfinite(face)) || dot(face,face)<=1e-8 || !isfinite(vertexAlpha)) return false;
    float3 worldFace=normalize(mul(face,worldToObject));
    float3 weights=Triplanar::GetWeights(worldN,worldFace);
    if (!all(isfinite(weights)) || dot(weights,1.0)<=0.0) return false;
    float3 worldMapped=normalize(mul(normal,worldToObject));
    float mask=NativeProjectionTriplanar(textures.w,(metadata.w>>6u)&3u,worldPosition,worldDx,worldDy,weights,params.z,noise,false).x;
    float coverage=-params.x*mask+dot(worldMapped,normalize(direction))*vertexAlpha-params.w;
    if (!isfinite(coverage)) return false;
    if (scales.w<=0.5) {
        if (coverage>0.0) base=SrgbToLinear(max(tint.xyz,0.0));
        return true;
    }
    if (any(textures.xyz>=NativeProjectionTextureCapacity)) return false;
    float diffuseScale=scales.x*params.z, detailScale=scales.y*params.z;
    float3 projectedColor=NativeProjectionTriplanar(textures.x,metadata.w&3u,worldPosition,worldDx,worldDy,weights,diffuseScale,noise,true).xyz;
    // Texture view conversion bit is recorded alongside the address modes.
    if ((metadata.w&256u)!=0u) projectedColor=NativeProjectionEncode(projectedColor);
    projectedColor=SrgbToLinear(max(projectedColor*tint.xyz*material.projected.xyz,0.0));
    float3 projectedNormal=NativeProjectionTriplanar(textures.y,(metadata.w>>2u)&3u,worldPosition,worldDx,worldDy,weights,diffuseScale,noise,true).xyz*2.0-1.0;
    float3 detail=NativeProjectionTriplanar(textures.z,(metadata.w>>4u)&3u,worldPosition,worldDx,worldDy,weights,detailScale,noise,true).xyz*2.0-1.0;
    float3 combined=detail*float3(1,1,projectedNormal.z)+float3(projectedNormal.xy,0);
    if (!all(isfinite(combined)) || dot(combined,combined)<=1e-8 || !all(isfinite(projectedColor))) return false;
    combined=normalize(combined);
    float3 projected=normalize(t*combined.x+b*combined.y+n*combined.z);
    projected=ReorientNormal(projected,normal,n);
    float weight=smoothstep(0.0,1.0,5.0*(0.1+coverage));
    float3 blendedNormal=lerp(normal,projected,weight);
    if (!all(isfinite(blendedNormal)) || dot(blendedNormal,blendedNormal)<=1e-8) return false;
    normal=normalize(blendedNormal);
    base=lerp(base,projectedColor,weight);
    packed.xyw=lerp(packed.xyw,float3(material.surface.w,0.0,material.projected.w),weight);
    projectedWeight=weight;
    return true;
}
#endif

#ifndef CSHADER
#define CSHADER
#endif
#include "Common/Color.hlsli"
#include "Common/GBuffer.hlsli"
#include "RedstoneGI/Composition.hlsli"
Texture2D<float4> NormalRoughness : register(t0);
RWTexture2D<float4> Legacy : register(u0);
cbuffer Settings : register(b0) { uint2 Size; float Strength; uint Padding; };
[numthreads(8,8,1)] void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;
    float4 nr=NormalRoughness[pixel];
    float3 normal=normalize(mul((float3x3)FrameBuffer::CameraViewInverse[0],GBuffer::DecodeNormal(nr.xy)));
    float3 diffuse,specular; float2 visibility;
    SampleRedstone(pixel,normal,1-nr.z,diffuse,specular,visibility);
    float3 value=(diffuse+specular)*Strength;
    Legacy[pixel]=float4(all(isfinite(value)) ? value : 0,1);
}

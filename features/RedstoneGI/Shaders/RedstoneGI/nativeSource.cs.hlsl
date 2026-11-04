#ifndef CSHADER
#define CSHADER
#endif
#include "Common/Color.hlsli"
Texture2D<float3> DiffuseSource : register(t0);
Texture2D<float3> SpecularSource : register(t1);
RWTexture2D<float4> InitialRadiance : register(u0);
cbuffer SourceSettings : register(b13) { uint2 NativeSize; uint2 Padding; };
[numthreads(8,8,1)] void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= NativeSize)) return;
    float3 diffuse = DiffuseSource[pixel];
    if (ENABLE_LL) diffuse = Color::SceneGammaToLinear(diffuse);
    float3 value = Color::IrradianceToLinear(diffuse) + SpecularSource[pixel];
    InitialRadiance[pixel] = float4(all(isfinite(value)) ? min(max(value, 0), 65504.0) : 0, 0);
}

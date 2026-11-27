#include "Common/NativeCubeDirection.hlsli"

// Input acquisition only. No temporal history, material lighting, inference,
// specular convolution or environment publication is performed by FXC.
Texture2D<float4> SceneColor : register(t0);
Texture2D<float> SceneDepth : register(t1);
RWTexture2DArray<float4> Raw : register(u0);
cbuffer Input : register(b0)
{
    row_major float4x4 View;
    row_major float4x4 Projection;
    row_major float4x4 InverseViewProjection;
    float4 DynamicResolution;
    float FarDepth;
    uint Size;
    uint2 Padding;
};
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Size) || id.z >= 6) return;
    Raw[id] = 0;
    float3 v = mul((float3x3)View, -NativeCubeDirection(id, Size));
    if (v.z >= 0) return;
    float4 clip = mul(Projection, float4(v,0));
    if (!all(isfinite(clip)) || abs(clip.w) < 1e-8) return;
    float2 uv = clip.xy / clip.w * float2(0.5,-0.5) + 0.5;
    if (any(uv < 0) || any(uv >= 1)) return;
    uint w,h,dw,dh;
    SceneColor.GetDimensions(w,h);
    SceneDepth.GetDimensions(dw,dh);
    if (w != dw || h != dh || any(DynamicResolution.xy <= 0) || any(DynamicResolution.xy > 1)) return;
    uint2 p = min(uint2(uv * DynamicResolution.xy * float2(w,h)), uint2(w-1,h-1));
    float depth = SceneDepth.Load(int3(p,0));
    if (!isfinite(depth) || depth < 0 || depth > 1) return;
    bool sky = abs(depth-FarDepth) <= 1e-7;
    float distance=0;
    if (!sky) {
        float4 world = mul(InverseViewProjection, float4(uv*float2(2,-2)+float2(-1,1),depth,1));
        if (!all(isfinite(world)) || abs(world.w) < 1e-8) return;
        distance=length(world.xyz/world.w);
        if(!isfinite(distance)||distance<16.5||distance>65504.0*1024.0)return;
    }
    float3 c = SceneColor.Load(int3(p,0)).rgb;
    if (all(isfinite(c))) Raw[id] = float4(clamp(c,0,65504), sky ? -1.0 : distance/1024.0);
}

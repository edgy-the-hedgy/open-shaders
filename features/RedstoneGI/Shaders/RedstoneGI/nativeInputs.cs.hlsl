#ifndef CSHADER
#define CSHADER
#endif
#include "Common/Color.hlsli"
#include "Common/ReverseZ.hlsli"
Texture2D<float> SceneDepth : register(t0);
Texture2D<float4> NormalRoughness : register(t1);
Texture2D<float3> SceneColor : register(t2);
Texture2D<float3> DiffuseAlbedo : register(t3);
Texture2D<float3> SpecularAlbedo : register(t4);
Texture2D<float4> NativeMotion : register(t5);
RWTexture2D<float> LinearDepth : register(u0);
RWTexture2D<float4> NormalOutput : register(u1);
RWTexture2D<float4> LinearRadiance : register(u2);
RWTexture2D<float4> DiffuseOutput : register(u3);
RWTexture2D<float4> SpecularOutput : register(u4);
RWTexture2D<float4> MotionOutput : register(u5);
cbuffer SnapshotSettings : register(b13)
{
    uint2 NativeSize;
    float SourceStrength;
    uint Padding;
};
[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= NativeSize)) return;
    float depth = FrameBuffer::ToStandardDepth(SceneDepth[pixel]);
    depth = SharedData::CameraData.w / (-depth * SharedData::CameraData.z + SharedData::CameraData.x);
    LinearDepth[pixel] = isfinite(depth) && depth > 0 ? depth : 0;
    NormalOutput[pixel] = NormalRoughness[pixel];
    float3 radiance = ENABLE_LL ? Color::SceneGammaToLinear(SceneColor[pixel]) : Color::RadianceToLinear(SceneColor[pixel]);
    LinearRadiance[pixel] = float4(all(isfinite(radiance)) ? min(max(0, radiance) * SourceStrength, 65504.0) : 0, 0);
    DiffuseOutput[pixel] = float4(DiffuseAlbedo[pixel], 0);
    SpecularOutput[pixel] = float4(SpecularAlbedo[pixel], 0);
    MotionOutput[pixel] = NativeMotion[pixel];
}

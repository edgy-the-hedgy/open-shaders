#ifndef CSHADER
#define CSHADER
#endif
#include "Common/Color.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/ReverseZ.hlsli"
#include "RedstoneGI/Composition.hlsli"
Texture2D<float4> Lighting : register(t0);
Texture2D<float4> Indirect : register(t1);
Texture2D<float4> NormalRoughness : register(t2);
RWTexture2D<float4> Scene : register(u0);
cbuffer Publication : register(b0) { uint2 Size; uint DirectSignal; float DirectBlend; uint LegacyIndirect; float IndirectStrength; uint2 Padding; };
[numthreads(8,8,1)] void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;
    float4 first = Lighting[pixel], second = Indirect[pixel];
    bool hasFirst = first.a > 0.5 && all(isfinite(first));
    bool hasSecond = DirectSignal && second.a > 0.5 && all(isfinite(second));
    if (!hasFirst && !hasSecond) return;
    float4 scene = Scene[pixel];
    float3 sceneLinear = Color::SceneGammaToLinear(scene.rgb);
    if (DirectSignal) {
        if (hasFirst) sceneLinear = lerp(sceneLinear, ENABLE_LL ? Color::GamutTransform(first.rgb) : first.rgb, saturate(DirectBlend));
        if (hasSecond) sceneLinear += ENABLE_LL ? Color::GamutTransform(second.rgb) : second.rgb;
        if (hasFirst && LegacyIndirect && DirectBlend > 0) {
            float4 nr = NormalRoughness[pixel];
            float3 normal = normalize(mul((float3x3)FrameBuffer::CameraViewInverse[0], GBuffer::DecodeNormal(nr.xy)));
            float3 diffuse, specular; float2 visibility;
            SampleRedstone(pixel, normal, 1 - nr.z, diffuse, specular, visibility);
            sceneLinear += (diffuse + specular) * IndirectStrength * saturate(DirectBlend);
        }
    } else if (hasFirst) sceneLinear += first.rgb;
    float3 encoded = Color::SceneLinearToGamma(sceneLinear);
    if (all(isfinite(encoded))) Scene[pixel] = float4(encoded, scene.a);
}

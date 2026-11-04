#ifndef REDSTONE_NATIVE_GI_COMPAT
#define REDSTONE_NATIVE_GI_COMPAT
// Native DX12 compatibility: preserve RedstoneGI horizon equations while
// replacing the DX11-owned Color and SharedData global constant buffers.
#include "Common/Math.hlsli"
#include "RedstoneGI/NativeGBuffer.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/ReverseZ.hlsli"
// HLSL cbuffer members are unqualified globals, even when the buffer is
// declared inside a namespace. Use explicit native names rather than
// pretending they are SharedData:: members.
cbuffer NativeLighting : register(b5)
{
    float4 NativeCameraData;
    uint NativeEnableLinearLighting;
    uint NativeEnableACEScg;
    uint NativeRasterSourceEncoded;
    uint NativeLightingPadding;
}
namespace Color
{
    float NativePBRLightingScale() { return NativeEnableLinearLighting != 0u ? 1.0 : 0.65; }
    float RGBToLuminance(float3 rgb)
    {
        return NativeEnableACEScg != 0u
            ? dot(rgb, float3(0.2722287168, 0.6740817658, 0.0536895174))
            : dot(rgb, float3(0.2125, 0.7154, 0.0721));
    }
    float3 IrradianceToLinear(float3 rgb)
    {
        return NativeEnableLinearLighting != 0u ? rgb : pow(abs(rgb), 1.6);
    }
}
#endif

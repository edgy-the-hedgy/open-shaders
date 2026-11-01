#ifndef REDSTONE_NATIVE_GI_COMPAT
#define REDSTONE_NATIVE_GI_COMPAT
// Native DX12 compatibility: preserve RedstoneGI horizon equations while
// replacing the DX11-owned Color and SharedData global constant buffers.
#include "Common/Math.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/ReverseZ.hlsli"
namespace SharedData
{
    cbuffer NativeLighting : register(b5)
    {
        float4 CameraData;
        uint NativeEnableLinearLighting;
        uint NativeEnableACEScg;
        uint2 NativeLightingPadding;
    };
}
namespace Color
{
    static const float PBRLightingScale = SharedData::NativeEnableLinearLighting != 0u ? 1.0 : 0.65;
    float RGBToLuminance(float3 rgb)
    {
        return SharedData::NativeEnableACEScg != 0u
            ? dot(rgb, float3(0.2722287168, 0.6740817658, 0.0536895174))
            : dot(rgb, float3(0.2125, 0.7154, 0.0721));
    }
    float3 IrradianceToLinear(float3 rgb)
    {
        return SharedData::NativeEnableLinearLighting != 0u ? rgb : pow(abs(rgb), 1.6);
    }
}
#endif

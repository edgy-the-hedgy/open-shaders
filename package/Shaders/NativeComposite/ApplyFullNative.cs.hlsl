#include "Common/Color.hlsli"

Texture2D<float4> NativeFinalColor : register(t0);
RWTexture2D<float4> MainRW : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    NativeFinalColor.GetDimensions(width, height);
    if (any(id.xy >= uint2(width, height))) return;
    float4 native = NativeFinalColor.Load(int3(id.xy, 0));
    // Do not alter any raster-owned pixel or its alpha.
    if (native.a != 1.0 || !all(isfinite(native.rgb))) return;
    uint targetWidth, targetHeight;
    MainRW.GetDimensions(targetWidth, targetHeight);
    if (any(id.xy >= uint2(targetWidth, targetHeight))) return;
    MainRW[id.xy] = float4(Color::SceneLinearToGamma(native.rgb), 1.0);
}

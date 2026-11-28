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
    // Alpha describes native receiver/contribution validity, not completeness.
    // A valid receiver without contributions is explicitly native black.
    if ((native.a != 1.0 && native.a != 2.0) || !all(isfinite(native.rgb))) return;
    uint targetWidth, targetHeight;
    MainRW.GetDimensions(targetWidth, targetHeight);
    if (any(id.xy >= uint2(targetWidth, targetHeight))) return;
    // Native composition is linear Rec.709; kMAIN is engine gamma-domain
    // storage, even with ACEScg enabled. Enter the engine working gamut before
    // SceneLinearToGamma (which converts AP1 back to Rec.709 when enabled).
    // The later scene gamma decode will then enter the configured working gamut.
    float3 working = (ENABLE_LL && ENABLE_ACEScg) ? Color::GamutTransform(native.rgb) : native.rgb;
    MainRW[id.xy] = float4(Color::SceneLinearToGamma(working), 1.0);
}

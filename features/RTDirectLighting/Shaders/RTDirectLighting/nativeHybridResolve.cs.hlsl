// Diagnostic output-only DX11 handoff. All lighting and AO math is
// performed in DX12 by nativeHybridCompose.dxil; this merely writes the
// completed texture to the game's existing D3D11 scene target.
Texture2D<float4> NativeComposedScene : register(t0);
RWTexture2D<float4> SceneColor : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    uint width, height;
    NativeComposedScene.GetDimensions(width, height);
    if (any(pixel >= uint2(width, height))) return;
    uint targetWidth, targetHeight;
    SceneColor.GetDimensions(targetWidth, targetHeight);
    if (any(pixel >= uint2(targetWidth, targetHeight))) return;
    const float4 color = NativeComposedScene.Load(int3(pixel, 0));
    SceneColor[pixel] = all(isfinite(color)) ? color : float4(0, 0, 0, 1);
}

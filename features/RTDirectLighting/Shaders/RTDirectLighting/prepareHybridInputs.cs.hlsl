// Stage 4A: loss-minimizing transport of six raster inputs to DX12 shareable UAVs.
// The depth source is the live DX11 depth SRV; only .x is meaningful.
Texture2D<float4> SceneColor    : register(t0);
Texture2D<float>  SceneDepth    : register(t1);
Texture2D<float4> NormalRough   : register(t2);
Texture2D<float4> Albedo        : register(t3);
Texture2D<float4> Reflectance   : register(t4);
Texture2D<float4> MotionVectors : register(t5);
RWTexture2D<float4> OutScene    : register(u0);
RWTexture2D<float4> OutDepth    : register(u1);
RWTexture2D<float4> OutNormal   : register(u2);
RWTexture2D<float4> OutAlbedo   : register(u3);
RWTexture2D<float4> OutReflect  : register(u4);
RWTexture2D<float4> OutMotion   : register(u5);
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    uint width, height;
    OutScene.GetDimensions(width, height);
    if (pixel.x >= width || pixel.y >= height) return;
    const int3 p = int3(pixel, 0);
    OutScene[pixel] = SceneColor.Load(p);
    OutDepth[pixel] = float4(SceneDepth.Load(p), 0, 0, 0);
    OutNormal[pixel] = NormalRough.Load(p);
    OutAlbedo[pixel] = Albedo.Load(p);
    OutReflect[pixel] = Reflectance.Load(p);
    OutMotion[pixel] = MotionVectors.Load(p);
}

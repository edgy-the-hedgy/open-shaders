Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float> ShadowDepth : register(u0);

[numthreads(8,8,1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    uint width, height;
    ShadowDepth.GetDimensions(width,height);
    if (any(pixel >= uint2(width,height))) return;
    ShadowDepth[pixel] = SourceDepth.Load(int3(pixel,0));
}

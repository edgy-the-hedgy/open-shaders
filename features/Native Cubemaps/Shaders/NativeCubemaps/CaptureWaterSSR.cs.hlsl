// Input-only acquisition. Flat water does not need color, normal direction,
// the legacy ray-march parameters or previous-frame alpha feedback.
Texture2D<float4> Normal : register(t0);
Texture2D<float> Depth : register(t1);
RWTexture2D<float> RawDepth : register(u0);
RWTexture2D<float> Mask : register(u1);
cbuffer Capture : register(b0) { uint2 Size; float2 Scale; };
uint2 Coordinate(float2 uv,uint2 size)
{
    return min(uint2(uv*Scale*float2(size)),size-1u);
}
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID)
{
    if(any(id.xy>=Size))return;
    uint2 ns,ds;
    Normal.GetDimensions(ns.x,ns.y);Depth.GetDimensions(ds.x,ds.y);
    float2 uv=(float2(id.xy)+0.5)/float2(Size);
    RawDepth[id.xy]=Depth.Load(int3(Coordinate(uv,ds),0));
    Mask[id.xy]=Normal.Load(int3(Coordinate(uv,ns),0)).z;
}

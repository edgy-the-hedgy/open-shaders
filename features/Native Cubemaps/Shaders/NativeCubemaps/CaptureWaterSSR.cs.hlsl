// Input-only acquisition from the original image-space pass bindings.
Texture2D<float4> Normal : register(t0);
Texture2D<float4> Color : register(t1);
Texture2D<float4> Depth : register(t2);
RWTexture2D<float4> Raw : register(u0);
RWTexture2D<float> Mask : register(u1);
cbuffer Capture : register(b0) { uint2 Size; float2 Scale; };
cbuffer PerGeometry : register(b2) { float4 SSRParams; float3 DefaultNormal; };
uint2 Coordinate(float2 uv,uint2 size)
{
    return min(uint2(uv*Scale*float2(size)),size-1u);
}
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID)
{
    if(any(id.xy>=Size))return;
    uint2 ns,cs,ds;
    Normal.GetDimensions(ns.x,ns.y);Color.GetDimensions(cs.x,cs.y);Depth.GetDimensions(ds.x,ds.y);
    float2 uv=(float2(id.xy)+0.5)/float2(Size);
    Raw[id.xy]=float4(Color.Load(int3(Coordinate(uv,cs),0)).rgb,Depth.Load(int3(Coordinate(uv,ds),0)).x);
    Mask[id.xy]=Normal.Load(int3(Coordinate(uv,ns),0)).z;
    if(all(id.xy==0u)){
        Raw[uint2(0,Size.y)]=SSRParams;
        Raw[uint2(1,Size.y)]=float4(DefaultNormal,1.0);
    }
}

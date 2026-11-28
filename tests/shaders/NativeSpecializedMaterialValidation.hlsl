// Compile-only exercise of the v2 publication and selective consumer APIs.
// Not a packaged runtime shader; dispatch/fence tests remain outstanding.
#include "NativeSpecializedMaterial.hlsli"
RWByteAddressBuffer DirectoryOut : register(u0);
RWByteAddressBuffer PoolOut : register(u1);
RWByteAddressBuffer Counters : register(u2);
ByteAddressBuffer DirectoryIn : register(t0);
ByteAddressBuffer PoolIn : register(t1);
RWByteAddressBuffer Readback : register(u3);
cbuffer Inputs : register(b0) { uint Pixels; uint Capacity; };
[numthreads(64,1,1)]
void publishMain(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=Pixels) return;
    NativeSpecializedPayload p=(NativeSpecializedPayload)0;
    p.coatColorStrength=uint2(NativeSpecializedPackHalf2(float2(1.0,0.5)),0u);
    p.coatNormalRoughnessF0.x=NativeSpecializedPackOct(float3(0,0,1));
    p.glint=float4(1.0,2.0,3.0,4.0);
    uint mask=(id.x&1u)!=0u ? SpecializedCoat|SpecializedCoatNormal|SpecializedGlint : 0u;
    NativeSpecializedPublish(DirectoryOut,PoolOut,Counters,id.x,Capacity,mask,mask|SpecializedFuzz,p);
}
[numthreads(64,1,1)]
void readMain(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=Pixels) return;
    NativeSpecializedDirectory d=NativeSpecializedReadDirectory(DirectoryIn,id.x,Capacity);
    uint2 coat=NativeSpecializedReadPair(PoolIn,d,SpecializedCoat,0u);
    uint4 glint=NativeSpecializedReadGlint(PoolIn,d);
    Readback.Store4(id.x*32u,uint4(d.ordinal,d.present,d.valid,NativeSpecializedKnownAbsent(d,SpecializedFuzz)?1u:0u));
    Readback.Store4(id.x*32u+16u,uint4(asuint(NativeSpecializedUnpackHalf2(coat.x)),glint.xy));
}

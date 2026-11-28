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
    NativeSpecializedEvaluation e=(NativeSpecializedEvaluation)0;
    if((id.x&1u)!=0u) {
        NativeSpecializedSetSSS(e,float3(0.5,0.0,0.25),0.8);
        NativeSpecializedSetAnisotropy(e,-0.25);
        NativeSpecializedSetFuzz(e,float3(0.1,0.2,0.3),0.0,0.5);
        NativeSpecializedSetTangent(e,float3(1,0,0),true);
        NativeSpecializedSetCoatNormal(e,float3(0,0,1));
        NativeSpecializedSetEyeNormal(e,float3(0,1,0));
        NativeSpecializedSetGlint(e,float4(1.0,2.0,3.0,4.0));
        // Present but invalid: neighboring valid fields must survive.
        NativeSpecializedSetEyeNormal(e,float3(0,0,0));
    } else NativeSpecializedKnownMissing(e,SpecializedFuzz);
    NativeSpecializedPublish(DirectoryOut,PoolOut,Counters,id.x,Capacity,e.present,e.valid,e.payload);
}
[numthreads(64,1,1)]
void readMain(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=Pixels) return;
    NativeSpecializedDirectory d=NativeSpecializedReadDirectory(DirectoryIn,id.x,Capacity);
    uint2 fuzz=NativeSpecializedReadPair(PoolIn,d,SpecializedFuzz,56u);
    uint4 glint=NativeSpecializedReadGlint(PoolIn,d);
    Readback.Store4(id.x*32u,uint4(d.ordinal,d.present,d.valid,NativeSpecializedKnownAbsent(d,SpecializedFuzz)?1u:0u));
    Readback.Store4(id.x*32u+16u,uint4(asuint(NativeSpecializedUnpackHalf2(fuzz.x)),glint.xy));
}

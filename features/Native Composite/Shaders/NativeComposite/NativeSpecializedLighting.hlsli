#ifndef NATIVE_SPECIALIZED_LIGHTING_HLSLI
#define NATIVE_SPECIALIZED_LIGHTING_HLSLI
#include "NativeSpecializedMaterial.hlsli"
#include "NativeSurface.hlsli"
#include "Common/PBRMath.hlsli"

// Decoded independently addressable properties, not a multiplexed v1 sample.
struct NativeSpecializedLighting {
    uint present, valid, flags;
    float3 coatColor, coatNormal, sssColor, thinNormal, transmissionColor, fuzzColor, tangent, eyeNormal;
    float coatStrength, coatRoughness, coatF0, opacity, balance, frontWeight, backWeight, backRoughness;
    float2 specularControls;
    float transmissionThickness, fuzzStrength, fuzzRoughness, anisotropy;
    float4 interlayer, glint;
};
bool NativeLobeAvailable(NativeSpecializedLighting s,uint bit) { return (s.present&s.valid&bit)==bit; }
bool NativeLobeAbsent(NativeSpecializedLighting s,uint bit) { return (s.valid&~s.present&bit)==bit; }
NativeSpecializedLighting ReadSpecializedLighting(ByteAddressBuffer directory, ByteAddressBuffer pool,
    uint pixelOrdinal, uint capacity, uint enabled, float3 normal)
{
    NativeSpecializedLighting s=(NativeSpecializedLighting)0;
    // Neutral inputs do not certify absent or available properties.
    s.coatNormal=s.thinNormal=s.eyeNormal=normal;
    s.opacity=1.0; s.specularControls=1.0; s.backRoughness=1.0;
    if(enabled==0u) return s;
    NativeSpecializedDirectory d=NativeSpecializedReadDirectory(directory,pixelOrdinal,capacity);
    s.present=d.present; s.valid=d.valid;
    if(d.ordinal==NativeSpecializedNoRecord) return s;
    uint o=d.ordinal*NativeSpecializedPayloadStride;
    if(NativeSpecializedAvailable(d,SpecializedCoat)) {
        s.coatStrength=saturate(NativeSpecializedUnpackHalf2(pool.Load(o+4u)).y);
        float2 rf=NativeSpecializedUnpackHalf2(pool.Load(o+12u));
        s.coatRoughness=clamp(rf.x,0.045,1.0); s.coatF0=saturate(rf.y);
    }
    if(NativeSpecializedAvailable(d,SpecializedColoredCoat)) {
        s.coatColor=float3(NativeSpecializedUnpackHalf2(pool.Load(o)),NativeSpecializedUnpackHalf2(pool.Load(o+4u)).x);
    }
    if(NativeSpecializedAvailable(d,SpecializedCoatNormal)) s.coatNormal=NativeSpecializedUnpackOct(pool.Load(o+8u));
    if(NativeSpecializedAvailable(d,SpecializedInterlayer)) s.interlayer=float4(NativeSpecializedUnpackHalf2(pool.Load(o+16u)),NativeSpecializedUnpackHalf2(pool.Load(o+20u)));
    if(NativeSpecializedAvailable(d,SpecializedSSS)) {
        float2 bo=NativeSpecializedUnpackHalf2(pool.Load(o+28u));
        s.sssColor=float3(NativeSpecializedUnpackHalf2(pool.Load(o+24u)),bo.x); s.opacity=saturate(bo.y);
    }
    if(NativeSpecializedAvailable(d,SpecializedThin)) {
        s.thinNormal=NativeSpecializedUnpackOct(pool.Load(o+32u));
        s.balance=saturate(NativeSpecializedUnpackHalf2(pool.Load(o+36u)).x);
        s.flags|=pool.Load(o+92u)&SpecializedFoliageFlag;
    }
    if(NativeSpecializedAvailable(d,SpecializedFront)) s.frontWeight=saturate(NativeSpecializedUnpackHalf2(pool.Load(o+36u)).y);
    if(NativeSpecializedAvailable(d,SpecializedBack)) {
        float2 bw=NativeSpecializedUnpackHalf2(pool.Load(o+40u)); s.backWeight=saturate(bw.x); s.backRoughness=max(bw.y,0.0);
    }
    if(NativeSpecializedAvailable(d,SpecializedSpecularControls)) s.specularControls=saturate(NativeSpecializedUnpackHalf2(pool.Load(o+44u)));
    if(NativeSpecializedAvailable(d,SpecializedTransmission)) {
        float2 bt=NativeSpecializedUnpackHalf2(pool.Load(o+52u));
        s.transmissionColor=float3(NativeSpecializedUnpackHalf2(pool.Load(o+48u)),bt.x); s.transmissionThickness=max(bt.y,0.0);
    }
    if(NativeSpecializedAvailable(d,SpecializedFuzz)) {
        float2 bs=NativeSpecializedUnpackHalf2(pool.Load(o+60u));
        s.fuzzColor=float3(NativeSpecializedUnpackHalf2(pool.Load(o+56u)),bs.x); s.fuzzStrength=saturate(bs.y);
        s.fuzzRoughness=NativeSpecializedUnpackHalf2(pool.Load(o+64u)).x;
    }
    if(NativeSpecializedAvailable(d,SpecializedAnisotropy)) s.anisotropy=clamp(NativeSpecializedUnpackHalf2(pool.Load(o+64u)).y,-1.0,1.0);
    if(NativeSpecializedAvailable(d,SpecializedTangent)) {
        s.tangent=NativeSpecializedUnpackOct(pool.Load(o+68u));
        s.flags|=pool.Load(o+92u)&SpecializedNegativeHandednessFlag;
    }
    if(NativeSpecializedAvailable(d,SpecializedGlint)) s.glint=asfloat(pool.Load4(o+72u));
    if(NativeSpecializedAvailable(d,SpecializedEyeNormal)) s.eyeNormal=NativeSpecializedUnpackOct(pool.Load(o+88u));
    return s;
}
bool NativeSpecializedTransmissionValid(NativeSpecializedLighting s)
{
    // A supported partial transmission signal is independently useful.
    uint mask=SpecializedSSS|SpecializedFront|SpecializedBack|SpecializedTransmission;
    return NativeLobeAvailable(s,SpecializedSSS) ||
        (NativeLobeAvailable(s,SpecializedThin) && (s.present&s.valid&(SpecializedFront|SpecializedBack))!=0u) ||
        NativeLobeAbsent(s,mask);
}
float3 NativeSpecializedBaseDirection(float3 direction,NativeSpecializedLighting s)
{
    return NativeLobeAvailable(s,SpecializedInterlayer|SpecializedCoatNormal) && s.interlayer.x>0.0 ?
        NativeRefractDirection(direction,s.coatNormal,min(1.0,rcp(s.interlayer.x))) : direction;
}
PBR::FuzzLayer ResolveSpecializedFuzz(NativeSpecializedLighting s,float roughness,float3 n,float3 view,bool cacheLTC)
{
    PBR::FuzzLayer fuzz=(PBR::FuzzLayer)0;
    fuzz.specularThroughput=1.0;
    if(NativeLobeAvailable(s,SpecializedFuzz)) {
        fuzz.roughness=clamp(s.fuzzRoughness>=0.0?s.fuzzRoughness:roughness,0.01,1.0);
        fuzz.weight=s.fuzzStrength; fuzz.color=saturate(s.fuzzColor);
        float3 face=PBR::GetThinSurfaceNormal(n,view);
        float ndv=clamp(dot(face,view),1e-4,1.0);
        fuzz.weightedAlbedo=saturate(fuzz.weight*PBR::FuzzDirectionalAlbedo(ndv,fuzz.roughness));
        fuzz.specularThroughput=1.0-(s.fuzzRoughness<0.0?fuzz.weight:fuzz.weightedAlbedo);
        if(cacheLTC&&fuzz.weightedAlbedo>0.0) fuzz.ltc=PBR::CreateFuzzLTC(view,face,ndv,fuzz.roughness);
    }
    return fuzz;
}
// Fractions already include the split of the nonopaque share.
float3 NativeSpecializedScatterColor(NativeSpecializedLighting s,float3 albedo)
{
    return NativeLobeAvailable(s,SpecializedTransmission) ? max(s.transmissionColor,0.0) :
        (NativeLobeAvailable(s,SpecializedSSS)?max(s.sssColor,0.0):albedo);
}
float3 NativeSpecializedAnisoNormal(NativeSpecializedLighting s,float3 n,float3 view)
{
    if(!NativeLobeAvailable(s,SpecializedAnisotropy|SpecializedTangent)) return n;
    float3 t=s.tangent-n*dot(s.tangent,n);
    if(dot(t,t)<1e-8) return n;
    t=normalize(t); float3 b=normalize(cross(n,t));
    float3 axis=s.anisotropy>=0.0?b:t;
    float3 candidate=n-axis*dot(axis,view)*abs(s.anisotropy);
    return dot(candidate,candidate)>1e-8?normalize(candidate):n;
}
#endif

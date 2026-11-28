#ifndef NATIVE_SPECIALIZED_MATERIAL_HLSLI
#define NATIVE_SPECIALIZED_MATERIAL_HLSLI
// Matches NativePBR/SpecializedMaterialAbi.h. No v1 float tags.
static const uint NativeSpecializedVersion=2u;
static const uint NativeSpecializedDirectoryStride=12u;
static const uint NativeSpecializedPayloadStride=96u;
static const uint NativeSpecializedNoRecord=0xffffffffu;
static const uint NativeSpecializedKnownBits=0x7fffu;
static const uint SpecializedCoat=1u<<0u, SpecializedColoredCoat=1u<<1u,
    SpecializedCoatNormal=1u<<2u, SpecializedInterlayer=1u<<3u, SpecializedSSS=1u<<4u,
    SpecializedThin=1u<<5u, SpecializedFront=1u<<6u, SpecializedBack=1u<<7u,
    SpecializedFuzz=1u<<8u, SpecializedAnisotropy=1u<<9u, SpecializedTangent=1u<<10u,
    SpecializedGlint=1u<<11u, SpecializedEyeNormal=1u<<12u,
    SpecializedTransmission=1u<<13u, SpecializedSpecularControls=1u<<14u;
static const uint SpecializedFoliageFlag=1u, SpecializedNegativeHandednessFlag=2u;

struct NativeSpecializedDirectory { uint ordinal; uint present; uint valid; };
struct NativeSpecializedPayload {
    uint2 coatColorStrength;
    uint2 coatNormalRoughnessF0;
    uint2 interlayer;
    uint2 subsurface;
    uint4 thinSurface;
    uint2 transmission;
    uint2 fuzz;
    uint2 fuzzRoughnessAnisotropyTangent;
    float4 glint;
    uint eyeNormal;
    uint flags;
};

bool NativeSpecializedHalfValid(float2 value)
{
    return all(isfinite(value)) && all(abs(value)<=65504.0);
}
uint NativeSpecializedPackHalf2(float2 value)
{
    uint2 bits=f32tof16(value);
    return (bits.x&65535u)|(bits.y<<16u);
}
float2 NativeSpecializedUnpackHalf2(uint value)
{
    return f16tof32(uint2(value&65535u,value>>16u));
}
// Callers validate finite/nondegenerate world-space directions before packing.
bool NativeSpecializedDirectionValid(float3 v)
{
    return all(isfinite(v)) && dot(v,v)>1e-8 && isfinite(dot(v,v));
}
uint NativeSpecializedPackOct(float3 v)
{
    v/=abs(v.x)+abs(v.y)+abs(v.z);
    float2 oct=v.xy;
    if(v.z<0.0) oct=(1.0-abs(oct.yx))*float2(oct.x>=0.0?1.0:-1.0,oct.y>=0.0?1.0:-1.0);
    uint2 bits=(uint2)round(saturate(oct*0.5+0.5)*65535.0);
    return bits.x|(bits.y<<16u);
}
float3 NativeSpecializedUnpackOct(uint packed)
{
    float2 oct=float2(packed&65535u,packed>>16u)*(2.0/65535.0)-1.0;
    float3 v=float3(oct,1.0-abs(oct.x)-abs(oct.y));
    float t=saturate(-v.z);
    v.xy+=float2(v.x>=0.0?-t:t,v.y>=0.0?-t:t);
    return normalize(v);
}
// Evaluation-side accumulation. A failed authored property remains present
// but invalid; it cannot overwrite any neighboring property. Only the
// evaluator may establish known absence from authoritative material evidence.
struct NativeSpecializedEvaluation {
    NativeSpecializedPayload payload;
    uint present;
    uint valid;
};
void NativeSpecializedKnownMissing(inout NativeSpecializedEvaluation e,uint bit)
{
    e.present&=~bit;
    e.valid|=bit&NativeSpecializedKnownBits;
}
void NativeSpecializedMarkPresent(inout NativeSpecializedEvaluation e,uint bit)
{
    e.present|=bit&NativeSpecializedKnownBits;
    e.valid&=~bit;
}
void NativeSpecializedSetSSS(inout NativeSpecializedEvaluation e,float3 color,float opacity)
{
    NativeSpecializedMarkPresent(e,SpecializedSSS);
    if(!NativeSpecializedHalfValid(color.xy)||!NativeSpecializedHalfValid(float2(color.z,opacity))) return;
    e.payload.subsurface=uint2(NativeSpecializedPackHalf2(color.xy),NativeSpecializedPackHalf2(float2(color.z,opacity)));
    e.valid|=SpecializedSSS;
}
void NativeSpecializedSetFuzz(inout NativeSpecializedEvaluation e,float3 color,float strength,float roughness)
{
    NativeSpecializedMarkPresent(e,SpecializedFuzz);
    if(!NativeSpecializedHalfValid(color.xy)||!NativeSpecializedHalfValid(float2(color.z,strength))||
        !NativeSpecializedHalfValid(float2(roughness,0.0))) return;
    e.payload.fuzz=uint2(NativeSpecializedPackHalf2(color.xy),NativeSpecializedPackHalf2(float2(color.z,strength)));
    // Fuzz roughness shares a word, not validity, with signed anisotropy.
    e.payload.fuzzRoughnessAnisotropyTangent.x=(e.payload.fuzzRoughnessAnisotropyTangent.x&0xffff0000u)|
        NativeSpecializedPackHalf2(float2(roughness,0.0));
    e.valid|=SpecializedFuzz;
}
void NativeSpecializedSetAnisotropy(inout NativeSpecializedEvaluation e,float anisotropy)
{
    NativeSpecializedMarkPresent(e,SpecializedAnisotropy);
    if(!isfinite(anisotropy)||abs(anisotropy)>1.0) return;
    e.payload.fuzzRoughnessAnisotropyTangent.x=(e.payload.fuzzRoughnessAnisotropyTangent.x&65535u)|
        NativeSpecializedPackHalf2(float2(0.0,anisotropy));
    e.valid|=SpecializedAnisotropy;
}
void NativeSpecializedSetTangent(inout NativeSpecializedEvaluation e,float3 tangent,bool negativeHandedness)
{
    NativeSpecializedMarkPresent(e,SpecializedTangent);
    if(!NativeSpecializedDirectionValid(tangent)) return;
    e.payload.fuzzRoughnessAnisotropyTangent.y=NativeSpecializedPackOct(tangent);
    e.payload.flags=(e.payload.flags&~SpecializedNegativeHandednessFlag)|
        (negativeHandedness?SpecializedNegativeHandednessFlag:0u);
    e.valid|=SpecializedTangent;
}
void NativeSpecializedSetCoatNormal(inout NativeSpecializedEvaluation e,float3 normal)
{
    NativeSpecializedMarkPresent(e,SpecializedCoatNormal);
    if(!NativeSpecializedDirectionValid(normal)) return;
    e.payload.coatNormalRoughnessF0.x=NativeSpecializedPackOct(normal);
    e.valid|=SpecializedCoatNormal;
}
void NativeSpecializedSetEyeNormal(inout NativeSpecializedEvaluation e,float3 normal)
{
    NativeSpecializedMarkPresent(e,SpecializedEyeNormal);
    if(!NativeSpecializedDirectionValid(normal)) return;
    e.payload.eyeNormal=NativeSpecializedPackOct(normal);
    e.valid|=SpecializedEyeNormal;
}
void NativeSpecializedSetGlint(inout NativeSpecializedEvaluation e,float4 parameters)
{
    NativeSpecializedMarkPresent(e,SpecializedGlint);
    if(!all(isfinite(parameters))) return;
    e.payload.glint=parameters;
    e.valid|=SpecializedGlint;
}
void NativeSpecializedSetCoat(inout NativeSpecializedEvaluation e,float strength,float roughness,float f0)
{
    NativeSpecializedMarkPresent(e,SpecializedCoat);
    if(!NativeSpecializedHalfValid(float2(roughness,f0))||!isfinite(strength)||strength<0.0||strength>1.0) return;
    e.payload.coatColorStrength.y=(e.payload.coatColorStrength.y&65535u)|NativeSpecializedPackHalf2(float2(0.0,strength));
    e.payload.coatNormalRoughnessF0.y=NativeSpecializedPackHalf2(float2(roughness,f0));
    e.valid|=SpecializedCoat;
}
void NativeSpecializedSetColoredCoat(inout NativeSpecializedEvaluation e,float3 color)
{
    NativeSpecializedMarkPresent(e,SpecializedColoredCoat);
    if(!NativeSpecializedHalfValid(color.xy)||!NativeSpecializedHalfValid(float2(color.z,0.0))) return;
    e.payload.coatColorStrength.x=NativeSpecializedPackHalf2(color.xy);
    e.payload.coatColorStrength.y=(e.payload.coatColorStrength.y&0xffff0000u)|NativeSpecializedPackHalf2(float2(color.z,0.0));
    e.valid|=SpecializedColoredCoat;
}
void NativeSpecializedSetInterlayer(inout NativeSpecializedEvaluation e,float ior,float depth,float displacement,float opticalThickness)
{
    NativeSpecializedMarkPresent(e,SpecializedInterlayer);
    if(ior<=0.0||!NativeSpecializedHalfValid(float2(ior,depth))||!NativeSpecializedHalfValid(float2(displacement,opticalThickness))) return;
    e.payload.interlayer=uint2(NativeSpecializedPackHalf2(float2(ior,depth)),NativeSpecializedPackHalf2(float2(displacement,opticalThickness)));
    e.valid|=SpecializedInterlayer;
}
void NativeSpecializedSetThin(inout NativeSpecializedEvaluation e,float3 normal,float balance,bool foliage)
{
    NativeSpecializedMarkPresent(e,SpecializedThin);
    if(!NativeSpecializedDirectionValid(normal)||!isfinite(balance)||balance<0.0||balance>1.0) return;
    e.payload.thinSurface.x=NativeSpecializedPackOct(normal);
    e.payload.thinSurface.y=(e.payload.thinSurface.y&0xffff0000u)|NativeSpecializedPackHalf2(float2(balance,0.0));
    e.payload.flags=(e.payload.flags&~SpecializedFoliageFlag)|(foliage?SpecializedFoliageFlag:0u);
    e.valid|=SpecializedThin;
}
void NativeSpecializedSetFront(inout NativeSpecializedEvaluation e,float weight)
{
    NativeSpecializedMarkPresent(e,SpecializedFront);
    if(!isfinite(weight)||weight<0.0||weight>1.0) return;
    e.payload.thinSurface.y=(e.payload.thinSurface.y&65535u)|NativeSpecializedPackHalf2(float2(0.0,weight));
    e.valid|=SpecializedFront;
}
void NativeSpecializedSetBack(inout NativeSpecializedEvaluation e,float weight,float roughnessScale)
{
    NativeSpecializedMarkPresent(e,SpecializedBack);
    if(weight<0.0||weight>1.0||roughnessScale<0.0||!NativeSpecializedHalfValid(float2(weight,roughnessScale))) return;
    e.payload.thinSurface.z=NativeSpecializedPackHalf2(float2(weight,roughnessScale));
    e.valid|=SpecializedBack;
}
void NativeSpecializedSetSpecularControls(inout NativeSpecializedEvaluation e,float2 weight)
{
    NativeSpecializedMarkPresent(e,SpecializedSpecularControls);
    if(!all(isfinite(weight))||any(weight<0.0)||any(weight>1.0)) return;
    e.payload.thinSurface.w=NativeSpecializedPackHalf2(weight);
    e.valid|=SpecializedSpecularControls;
}
void NativeSpecializedSetTransmission(inout NativeSpecializedEvaluation e,float3 color,float thickness)
{
    NativeSpecializedMarkPresent(e,SpecializedTransmission);
    if(thickness<0.0||!NativeSpecializedHalfValid(color.xy)||!NativeSpecializedHalfValid(float2(color.z,thickness))) return;
    e.payload.transmission=uint2(NativeSpecializedPackHalf2(color.xy),NativeSpecializedPackHalf2(float2(color.z,thickness)));
    e.valid|=SpecializedTransmission;
}
NativeSpecializedDirectory NativeSpecializedSanitize(uint ordinal,uint present,uint valid,uint capacity)
{
    NativeSpecializedDirectory d;
    d.ordinal=NativeSpecializedNoRecord;
    d.present=present&NativeSpecializedKnownBits;
    d.valid=valid&NativeSpecializedKnownBits;
    if((d.present&d.valid)!=0u) {
        if(ordinal<capacity && ordinal!=NativeSpecializedNoRecord) d.ordinal=ordinal;
        else d.valid&=~d.present;
    }
    return d;
}
// Returns a range from a saturating counter. All participating lanes must
// call together. One successful CAS per nonempty wave; retries on contention.
// No unbounded increment, no out-of-capacity store, no fairness guarantee.
uint NativeSpecializedReserve(RWByteAddressBuffer counters,uint request,uint capacity)
{
    uint start=capacity;
    if(WaveIsFirstLane() && request!=0u) {
        uint observed;
        counters.InterlockedAdd(0u,0u,observed);
        [allow_uav_condition]
        while(observed<capacity) {
            uint desired=observed+min(request,capacity-observed);
            uint previous;
            counters.InterlockedCompareExchange(0u,observed,desired,previous);
            if(previous==observed) { start=observed; break; }
            observed=previous;
        }
    }
    return WaveReadLaneFirst(start);
}
void NativeSpecializedStorePayload(RWByteAddressBuffer pool,uint offset,NativeSpecializedPayload p)
{
    pool.Store4(offset,uint4(p.coatColorStrength,p.coatNormalRoughnessF0));
    pool.Store4(offset+16u,uint4(p.interlayer,p.subsurface));
    pool.Store4(offset+32u,p.thinSurface);
    pool.Store4(offset+48u,uint4(p.transmission,p.fuzz));
    pool.Store4(offset+64u,uint4(p.fuzzRoughnessAnisotropyTangent,asuint(p.glint.xy)));
    pool.Store4(offset+80u,uint4(asuint(p.glint.zw),p.eyeNormal,p.flags&3u));
}
// Call for every in-bounds pixel, even invalid receivers (zero masks).
// Directory/pool/counters are distinct UAVs, reset counters before dispatch;
// transition/barrier all publications and fence before any reader uses them.
void NativeSpecializedPublish(RWByteAddressBuffer directory,RWByteAddressBuffer pool,
    RWByteAddressBuffer counters,uint pixelOrdinal,uint capacity,uint present,uint valid,
    NativeSpecializedPayload payload)
{
    uint needed=(present&valid)&NativeSpecializedKnownBits;
    bool wants=needed!=0u;
    uint count=WaveActiveCountBits(wants);
    uint prefix=WavePrefixCountBits(wants);
    uint start=NativeSpecializedReserve(counters,count,capacity);
    bool admitted=wants && start<capacity && prefix<capacity-start;
    uint ordinal=admitted?start+prefix:NativeSpecializedNoRecord;
    NativeSpecializedDirectory d=NativeSpecializedSanitize(ordinal,present,valid,capacity);
    if(admitted) NativeSpecializedStorePayload(pool,ordinal*NativeSpecializedPayloadStride,payload);
    directory.Store3(pixelOrdinal*NativeSpecializedDirectoryStride,uint3(d.ordinal,d.present,d.valid));
    bool overflow=wants&&!admitted;
    uint dropped=WaveActiveCountBits(overflow);
    if(WaveIsFirstLane()&&dropped!=0u) counters.InterlockedAdd(4u,dropped);
    if(WaveActiveAnyTrue(overflow)) {
        [unroll] for(uint bit=0u;bit<15u;++bit) {
            uint lost=WaveActiveCountBits(overflow && (needed&(1u<<bit))!=0u);
            if(WaveIsFirstLane()&&lost!=0u) counters.InterlockedAdd(8u+bit*4u,lost);
        }
    }
}
NativeSpecializedDirectory NativeSpecializedReadDirectory(ByteAddressBuffer directory,uint pixelOrdinal,uint capacity)
{
    uint3 packed=directory.Load3(pixelOrdinal*NativeSpecializedDirectoryStride);
    return NativeSpecializedSanitize(packed.x,packed.y,packed.z,capacity);
}
bool NativeSpecializedAvailable(NativeSpecializedDirectory d,uint mask)
{
    return d.ordinal!=NativeSpecializedNoRecord && mask!=0u && (d.present&d.valid&mask)==mask;
}
bool NativeSpecializedKnownAbsent(NativeSpecializedDirectory d,uint mask)
{
    return mask!=0u && (d.valid&~d.present&mask)==mask;
}
// Selective reads: mask describes the exact field group being consumed.
// Offsets are compile-time ABI constants; the guard avoids invalid payload loads.
uint2 NativeSpecializedReadPair(ByteAddressBuffer pool,NativeSpecializedDirectory d,uint mask,uint offset)
{
    if(!NativeSpecializedAvailable(d,mask) || offset>88u || (offset&3u)!=0u) return uint2(0u,0u);
    return pool.Load2(d.ordinal*NativeSpecializedPayloadStride+offset);
}
uint4 NativeSpecializedReadGlint(ByteAddressBuffer pool,NativeSpecializedDirectory d)
{
    if(!NativeSpecializedAvailable(d,SpecializedGlint)) return uint4(0u,0u,0u,0u);
    return pool.Load4(d.ordinal*NativeSpecializedPayloadStride+72u);
}
#endif

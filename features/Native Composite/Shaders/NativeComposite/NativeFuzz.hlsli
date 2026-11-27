#ifndef NATIVE_FUZZ_HLSLI
#define NATIVE_FUZZ_HLSLI
#include "Common/PBRMath.hlsli"
#include "NativeSurface.hlsli"

PBR::FuzzLayer ResolveNativeFuzz(NativeCoatSample material, float roughness, float3 n, float3 view, bool cacheLTC)
{
    PBR::FuzzLayer fuzz=(PBR::FuzzLayer)0;
    fuzz.specularThroughput=1.0;
    if (HasNativeFuzz(material)) {
        float authoredRoughness=NativeFuzzRoughness(material);
        fuzz.roughness=clamp(authoredRoughness>=0.0 ? authoredRoughness : roughness,0.01,1.0);
        fuzz.weight=saturate(material.colorStrength.w);
        fuzz.color=saturate(material.colorStrength.rgb);
        float3 faceNormal=PBR::GetThinSurfaceNormal(n,view);
        float ndv=clamp(dot(faceNormal,view),1e-4,1.0);
        fuzz.weightedAlbedo=saturate(fuzz.weight*PBR::FuzzDirectionalAlbedo(ndv,fuzz.roughness));
        fuzz.specularThroughput=1.0-(authoredRoughness<0.0 ? fuzz.weight : fuzz.weightedAlbedo);
        if (cacheLTC && fuzz.weightedAlbedo>0.0)
            fuzz.ltc=PBR::CreateFuzzLTC(view,faceNormal,ndv,fuzz.roughness);
    }
    return fuzz;
}
#endif

#ifndef NATIVE_DIRECT_BRDF_HLSLI
#define NATIVE_DIRECT_BRDF_HLSLI
#include "NativeFuzz.hlsli"
#include "Common/LightingCommon.hlsli"
#include "Common/BRDF.hlsli"
void AccumulateBRDF(float4 base, float4 f0, float3 n, float3 view, float3 l, float ndl, float ndv, float a2,
    float3 incident, float heightVisibility, NativeCoatSample coat, PBR::FuzzLayer fuzz, uint foliageScatteringEnabled, inout float3 diffuse, inout float3 specular, inout float3 transmission)
{
    bool layered=HasNativeInterlayer(coat);
    if (!layered) incident*=heightVisibility;
    float3 coatView=view, coatLight=l;
    if (layered) {
        view=NativeBaseDirection(view,coat);
        l=NativeBaseDirection(l,coat);
        ndl=saturate(dot(n,l));
        ndv=max(saturate(dot(n,view)),1e-4);
    }
    // Non-thin scattering follows Common/PBR.hlsli, with native DXR visibility.
    // Keep scattering separate from reflected diffuse.
    if (HasNativeThinSurface(coat)) {
        float3 albedo=max(base.rgb,0.0)*(1.0-saturate(f0.a));
        float3 reflected,front,back;
        NativeThinAlbedos(albedo,coat,reflected,front,back);
        float weight=saturate(coat.f0Colored.x);
        float2 env=BRDF::EnvBRDF(sqrt(sqrt(a2)),saturate(abs(dot(n,view))));
        float3 kd=(1.0-saturate(f0.rgb*env.x+env.y)*weight)*(1.0-fuzz.weightedAlbedo);
        float signedCos=dot(n,l);
        float backVisibility=1.0-saturate(4.0*dot(coat.normalRoughness.xyz,l));
        diffuse+=reflected*saturate(signedCos)*incident*BRDF::Diffuse_Lambert()*kd;
        transmission+=(front*saturate(signedCos)+saturate(albedo*(1.0-saturate(coat.subsurface.w))+back)*saturate(-signedCos)*backVisibility)*incident*BRDF::Diffuse_Lambert()*kd;
        if (foliageScatteringEnabled!=0u && HasNativeFoliage(coat)) {
            // Shared canopy approximation is independent of baked material thickness.
            float scattering=saturate(-signedCos)*BRDF::Diffuse_Lambert()+GetFoliageTransmission(signedCos,dot(view,l));
            transmission+=max(base.rgb,0.0)*scattering*incident*kd;
        }
        float3 halfDelta=view+l;
        if (dot(halfDelta,halfDelta)>1e-8 && ndl>0.0) {
            float3 h=normalize(halfDelta);
            float roughness=sqrt(sqrt(a2));
            specular+=weight*fuzz.specularThroughput*BRDF::D_GGX(roughness,saturate(dot(n,h)))*BRDF::Vis_SmithJoint(roughness,ndv,max(ndl,1e-4))*BRDF::F_Schlick(f0.rgb,saturate(dot(view,h)))*incident*ndl;
        }
        if (fuzz.weightedAlbedo>0.0) {
            float3 sheen=fuzz.weightedAlbedo*fuzz.color*incident;
            specular+=sheen*PBR::FuzzLobe(l,fuzz.ltc);
            // Transmitted sheen belongs to the independent transmission signal.
            if (signedCos<0.0)
                transmission+=sheen*back*PBR::FuzzLobeTransmitted(l,fuzz.ltc)*backVisibility;
        }
        return;
    }
    if (HasNativeSubsurface(coat)) {
        float thickness=saturate(coat.subsurface.w);
        float forwardScatter=exp2(saturate(-dot(view,l))*12.234-12.234);
        float backScatter=saturate(ndl*thickness+(1.0-thickness))*0.5;
        float scatter=lerp(backScatter,1.0,forwardScatter)*(1.0-thickness);
        float2 env=BRDF::EnvBRDF(sqrt(sqrt(a2)),saturate(abs(dot(n,view))));
        float3 kd=1.0-saturate(f0.rgb*env.x+env.y);
        transmission+=coat.subsurface.rgb*scatter*incident*BRDF::Diffuse_Lambert()*kd*(1.0-fuzz.weightedAlbedo);
    }
    float3 halfDelta = view + l;
    if (dot(halfDelta,halfDelta) < 1e-8) return;
    float3 h = normalize(halfDelta);
    float ndh = saturate(dot(n,h)), vdh = saturate(dot(view,h));
    float3 fresnel = saturate(f0.rgb) + (1.0-saturate(f0.rgb))*pow(1.0-vdh,5.0);
    float dDenom = ndh*ndh*(a2-1.0)+1.0;
    float distribution = a2 / max(3.14159265*dDenom*dDenom,1e-7);
    float gV = ndl*sqrt(ndv*ndv*(1.0-a2)+a2);
    float gL = ndv*sqrt(ndl*ndl*(1.0-a2)+a2);
    float visibility = 0.5/max(gV+gL,1e-5);
    float3 irradiance = incident * ndl * (layered ? heightVisibility : 1.0);
    float3 baseDiffuse = max(base.rgb,0.0)*(1.0-saturate(f0.a))*(1.0-fresnel)*(1.0/3.14159265)*irradiance;
    float3 baseSpecular = fresnel*distribution*visibility*irradiance;
    if (HasNativeCoat(coat)) {
        float3 cn = normalize(coat.normalRoughness.xyz);
        float cnl = saturate(dot(cn,coatLight)), cnv = max(saturate(abs(dot(cn,coatView))),1e-4);
        float3 coatHalfDelta=coatView+coatLight;
        float3 coatHalf=dot(coatHalfDelta,coatHalfDelta)>1e-8 ? normalize(coatHalfDelta) : cn;
        float cnh = saturate(dot(cn,coatHalf));
        float coatVdh=saturate(dot(coatView,coatHalf));
        float ca = coat.normalRoughness.w * coat.normalRoughness.w;
        float ca2 = ca*ca;
        float3 cf = saturate(coat.f0Colored.rgb) + (1.0-saturate(coat.f0Colored.rgb))*pow(1.0-coatVdh,5.0);
        float cd = cnh*cnh*(ca2-1.0)+1.0;
        float cgV = cnl*sqrt(cnv*cnv*(1.0-ca2)+ca2);
        float cgL = cnv*sqrt(cnl*cnl*(1.0-ca2)+ca2);
        float strength=saturate(coat.colorStrength.w);
        float3 attenuation=1.0-cf*strength;
        baseDiffuse *= attenuation;
        baseSpecular *= attenuation;
        if (coat.f0Colored.w != 0.0)
            baseDiffuse=lerp(baseDiffuse,coat.colorStrength.rgb*incident*cnl*(1.0/3.14159265),strength);
        baseSpecular += cf*(ca2/max(3.14159265*cd*cd,1e-7))*(0.5/max(cgV+cgL,1e-5))*incident*cnl*strength;
    }
    baseDiffuse*=1.0-fuzz.weightedAlbedo;
    baseSpecular*=fuzz.specularThroughput;
    if (fuzz.weightedAlbedo>0.0)
        baseSpecular+=fuzz.weightedAlbedo*fuzz.color*PBR::FuzzLobe(l,fuzz.ltc)*incident;
    diffuse += baseDiffuse;
    specular += baseSpecular;
}

#endif

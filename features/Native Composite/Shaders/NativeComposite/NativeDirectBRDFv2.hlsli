#ifndef NATIVE_DIRECT_BRDF_V2_HLSLI
#define NATIVE_DIRECT_BRDF_V2_HLSLI
#include "NativeSpecializedLighting.hlsli"
#include "Common/BRDF.hlsli"
#include "Common/LightingCommon.hlsli"
float3 SpecializedGGX(float3 f0,float3 n,float3 view,float3 l,float roughness,NativeSpecializedLighting s,bool anisotropic)
{
    float3 hv=view+l;
    float ndl=saturate(dot(n,l)),ndv=max(saturate(dot(n,view)),1e-4);
    if(ndl<=0.0||dot(hv,hv)<1e-8) return 0.0;
    float3 h=normalize(hv);
    float ndh=saturate(dot(n,h));
    float d=BRDF::D_GGX(roughness,ndh);
    if(anisotropic&&NativeLobeAvailable(s,SpecializedAnisotropy|SpecializedTangent)) {
        float3 t=s.tangent-n*dot(s.tangent,n);
        if(dot(t,t)>1e-8) {
            t=normalize(t); float3 b=normalize(cross(n,t));
            float a=max(roughness*roughness,0.002);
            float aspect=sqrt(1.0-0.9*abs(s.anisotropy));
            float ax=s.anisotropy>=0.0?a/aspect:a*aspect,ay=s.anisotropy>=0.0?a*aspect:a/aspect;
            float q=pow(dot(h,t)/ax,2.0)+pow(dot(h,b)/ay,2.0)+ndh*ndh;
            d=rcp(max(3.14159265*ax*ay*q*q,1e-7));
        }
    }
    return d*BRDF::Vis_SmithJoint(roughness,ndv,max(ndl,1e-4))*BRDF::F_Schlick(f0,saturate(dot(view,h)))*ndl;
}
void AccumulateSpecializedBRDF(float4 base,float4 f0,float3 n,float3 view,float3 l,float roughness,
    float3 incident,float heightVisibility,NativeSpecializedLighting s,PBR::FuzzLayer fuzz,uint foliageEnabled,
    inout float3 diffuse,inout float3 specular,inout float3 transmission)
{
    float3 coatView=view,coatLight=l;
    view=NativeSpecializedBaseDirection(view,s); l=NativeSpecializedBaseDirection(l,s);
    float signedCos=dot(n,l),ndl=saturate(signedCos);
    float3 albedo=max(base.rgb,0.0)*(1.0-saturate(f0.a));
    bool thin=NativeLobeAvailable(s,SpecializedThin);
    float front=NativeLobeAvailable(s,SpecializedFront)?s.frontWeight:0.0;
    float back=NativeLobeAvailable(s,SpecializedBack)?s.backWeight:0.0;
    float opacity=thin?saturate(1.0-front-back):s.opacity;
    float control=NativeLobeAvailable(s,SpecializedSpecularControls)?(signedCos>=0.0?s.specularControls.x:s.specularControls.y):1.0;
    float2 env=BRDF::EnvBRDF(roughness,saturate(abs(dot(n,view))));
    float3 kd=1.0-saturate(f0.rgb*env.x+env.y)*control;
    float3 dd=albedo*opacity*ndl*BRDF::Diffuse_Lambert()*kd*(1.0-fuzz.weightedAlbedo);
    float3 ds=SpecializedGGX(f0.rgb,n,view,l,roughness,s,true)*control*fuzz.specularThroughput;
    float3 dt=0.0;
    if(thin) {
        float bias=1.0-saturate(4.0*dot(s.thinNormal,l));
        dt=NativeSpecializedScatterColor(s,albedo)*(front*ndl+back*saturate(-signedCos)*bias)*BRDF::Diffuse_Lambert()*kd*(1.0-fuzz.weightedAlbedo);
        if(foliageEnabled!=0u&&(s.flags&SpecializedFoliageFlag)!=0u)
            dt+=albedo*GetFoliageTransmission(signedCos,dot(view,l))*kd*(1.0-fuzz.weightedAlbedo);
    } else if(NativeLobeAvailable(s,SpecializedSSS)) {
        float forward=exp2(saturate(-dot(view,l))*12.234-12.234);
        float scatter=lerp(saturate(ndl*s.opacity+1.0-s.opacity)*0.5,1.0,forward)*(1.0-s.opacity);
        dt=s.sssColor*scatter*BRDF::Diffuse_Lambert()*kd*(1.0-fuzz.weightedAlbedo);
    }
    if(fuzz.weightedAlbedo>0.0) ds+=fuzz.weightedAlbedo*fuzz.color*PBR::FuzzLobe(l,fuzz.ltc);
    dd*=heightVisibility; ds*=heightVisibility; dt*=heightVisibility;
    if(NativeLobeAvailable(s,SpecializedCoat)) {
        float3 cn=s.coatNormal;
        float2 cb=BRDF::EnvBRDF(s.coatRoughness,saturate(abs(dot(cn,coatView))));
        float reflectance=saturate(s.coatF0*cb.x+cb.y);
        float attenuation=1.0-s.coatStrength*reflectance;
        dd*=attenuation; ds*=attenuation; dt*=attenuation;
        if(NativeLobeAvailable(s,SpecializedColoredCoat)) {
            dd=dd*(1.0-s.coatStrength)+s.coatColor*(1.0-reflectance)*s.coatStrength*saturate(dot(cn,coatLight))*BRDF::Diffuse_Lambert();
            ds*=1.0-s.coatStrength; dt*=1.0-s.coatStrength;
        }
        ds+=SpecializedGGX(s.coatF0.xxx,cn,coatView,coatLight,s.coatRoughness,s,false)*s.coatStrength;
    }
    diffuse+=dd*incident; specular+=ds*incident; transmission+=dt*incident;
}
#endif

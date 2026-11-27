#ifndef NATIVE_FORWARD_ENVIRONMENT_HLSLI
#define NATIVE_FORWARD_ENVIRONMENT_HLSLI
#include "Common/Color.hlsli"
#include "Common/Permutation.hlsli"

// Only filtered NativeCubemaps publications are exposed here. Never define
// DYNAMIC_CUBEMAPS or run its producer for the compatibility path.
namespace NativeForwardEnvironment
{
    TextureCube<float4> Reflection : register(t30);
    TextureCube<float4> Base : register(t31);
    bool Available()
    {
        const uint flags=Permutation::ExtraShaderDescriptor;
        if ((flags&Permutation::ExtraFlags::InWorld)==0u ||
            (flags&Permutation::ExtraFlags::InReflection)!=0u) return false;
        uint w,h,mips;Base.GetDimensions(0,w,h,mips);
        return w>0u&&h>0u&&mips>0u;
    }
    float3 Sample(SamplerState sampler,float3 direction,float roughness,float skyVisibility)
    {
        uint w,h,mips;Base.GetDimensions(0,w,h,mips);
        const float mip=saturate(roughness)*float(max(mips,1u)-1u);
        float3 base=Color::IrradianceToLinear(Base.SampleLevel(sampler,direction,mip).rgb);
        if(SharedData::InInterior||SharedData::HideSky) return base;
        float3 reflection=Color::IrradianceToLinear(Reflection.SampleLevel(sampler,direction,mip).rgb);
        return lerp(base,reflection,saturate(skyVisibility));
    }
}
#endif

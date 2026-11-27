#ifndef NATIVE_INDIRECT_BRDF_HLSLI
#define NATIVE_INDIRECT_BRDF_HLSLI

#include "Common/BRDF.hlsli"
#include "Common/Shading.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

namespace NativeIndirect
{
    float EvaluateIrradiance(float4 sh, float3 direction)
    {
        // The zonal-axis approximation is undefined for a constant or black environment.
        if (sh.x <= 1e-6 || dot(sh.yzw, sh.yzw) <= 1e-12)
            return max(0.0, dot(sh, SphericalHarmonics::EvaluateCosineLobe(direction))) / Math::PI;
        return SphericalHarmonics::SHHallucinateZH3Irradiance(sh, direction) / Math::PI;
    }

    float3 EvaluateIrradiance(float4 r, float4 g, float4 b, float3 direction)
    {
        return float3(EvaluateIrradiance(r, direction), EvaluateIrradiance(g, direction), EvaluateIrradiance(b, direction));
    }

    struct LobeWeights
    {
        float3 diffuse;
        float3 specular;
    };

    LobeWeights EvaluateLobes(float3 baseColor, float3 f0, float metallic, float roughness, float ao, float ndv)
    {
        LobeWeights result;
        float2 brdf = BRDF::EnvBRDF(saturate(roughness), saturate(ndv));
        float3 reflected = saturate(saturate(f0) * brdf.x + brdf.y);
        float3 diffuseAlbedo = max(baseColor, 0.0) * (1.0 - saturate(metallic));
        result.diffuse = diffuseAlbedo * (1.0 - reflected) * MultiBounceAO(saturate(diffuseAlbedo), saturate(ao));
        result.specular = reflected * SpecularOcclusion(saturate(ndv), roughness * roughness, saturate(ao));
        return result;
    }
}

#endif

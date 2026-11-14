#ifndef NATIVE_PBR_MATERIAL_HLSLI
#define NATIVE_PBR_MATERIAL_HLSLI

#include "Common/PBRMath.hlsli"

namespace NativePBR
{
	static const uint MissingTexture = 0xffffffffu;
	static const uint VertexColorFlag = 8u;
	static const uint EmissionFlag = 64u;

	struct MaterialRecord
	{
		uint4 colorNormal;
		uint4 packedEmission;
		float4 uv;
		float4 emission;
		float4 specular;
		float4 scalar;
		uint4 flags;
		float4 reserved;
	};

	struct SurfaceInput
	{
		float2 uv;
		float2 uvDx;
		float2 uvDy;
		float3 tangent;
		float3 bitangent;
		float3 geometricNormal;
		float3 vertexColor;
		float facing;
	};

	struct SurfaceMaterial
	{
		float3 baseColor;
		float3 normal;
		float3 f0;
		float3 emission;
		float roughness;
		float metallic;
		float ao;
		float opacity;
	};

	float3 SrgbToLinear(float3 value)
	{
		value = max(value, 0.0f);
		return lerp(value / 12.92f, pow((value + 0.055f) / 1.055f, 2.4f), value > 0.04045f);
	}

	float3 DecodeNormal(SurfaceInput surface, float3 sampled)
	{
		float normalLengthSq = dot(surface.geometricNormal, surface.geometricNormal);
		float3 n = normalLengthSq > 1e-8f ? surface.geometricNormal * rsqrt(normalLengthSq) : float3(0.0f, 0.0f, 1.0f);
		n *= surface.facing < 0.0f ? -1.0f : 1.0f;
		float3 t = surface.tangent - n * dot(n, surface.tangent);
		float tangentLengthSq = dot(t, t);
		if (tangentLengthSq <= 1e-8f)
			return n;
		t *= rsqrt(tangentLengthSq);
		float handedness = dot(cross(n, t), surface.bitangent) < 0.0f ? -1.0f : 1.0f;
		float3 b = cross(n, t) * handedness;
		float3 tangentNormal = sampled * 2.0f - 1.0f;
		return normalize(t * tangentNormal.x + b * tangentNormal.y + n * tangentNormal.z);
	}

	SurfaceMaterial EvaluateOpaque(MaterialRecord material, SurfaceInput surface, float4 baseTexel,
		float4 normalTexel, float4 packedTexel, float4 emissionTexel)
	{
		SurfaceMaterial result;
		result.baseColor = SrgbToLinear(baseTexel.rgb);
		if ((material.flags.x & VertexColorFlag) != 0u)
			result.baseColor *= SrgbToLinear(surface.vertexColor);
		result.normal = DecodeNormal(surface, normalTexel.xyz);
		result.roughness = clamp(packedTexel.r * material.scalar.w, PBR::Constants::MinRoughness, PBR::Constants::MaxRoughness);
		result.metallic = saturate(packedTexel.g);
		result.ao = saturate(packedTexel.b);
		result.f0 = lerp(saturate(packedTexel.a * material.scalar.z).xxx, result.baseColor, result.metallic);
		result.opacity = saturate(baseTexel.a * material.scalar.x);
		result.emission = (material.flags.x & EmissionFlag) != 0u ?
			SrgbToLinear(emissionTexel.rgb) * material.emission.rgb * material.emission.a : 0.0f;
		return result;
	}
}

#endif

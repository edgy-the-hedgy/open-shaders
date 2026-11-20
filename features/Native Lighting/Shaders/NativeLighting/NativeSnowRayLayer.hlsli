#ifndef NATIVE_SNOW_RAY_LAYER_HLSLI
#define NATIVE_SNOW_RAY_LAYER_HLSLI

struct NativeSnowRayLayer
{
	uint4 textures[2];
	float4 projectedUVParams;
	float4 projectedUVParams2;
	float4 projectedUVParams3;
	float4 materialRGBScale;
	float4 directionAndSpace;
	uint4 flags;
};

StructuredBuffer<NativeSnowRayLayer> NativeSnowLayers : register(t10);

bool SampleNativeSnowTriplanar(uint2 location, uint samplerMode, float3 position, float3 weights,
	float scale, float footprint, out float4 sampled)
{
	sampled = 0.0f;
	if (!isfinite(scale) || scale <= 0.0f)
		return false;
	float lod = log2(max(footprint * scale, 1e-10f));
	float4 x, y, z;
	if (!SampleNativePBR(location, samplerMode, position.yz * scale, lod, x) ||
		!SampleNativePBR(location, samplerMode, position.xz * scale, lod, y) ||
		!SampleNativePBR(location, samplerMode, position.xy * scale, lod, z))
		return false;
	sampled = x * weights.x + y * weights.y + z * weights.z;
	return all(isfinite(sampled));
}

bool ApplyNativeProjectedSnow(uint instance, uint flags, TriangleSurface hitSurface, float2 barycentric,
	float3x4 objectToWorld, float footprint, bool pbrColorScale, NativePBR::SurfaceInput surface,
	inout NativePBR::SurfaceMaterial material)
{
	if ((flags & 16u) == 0u)
		return false;
	NativeSnowRayLayer snow = NativeSnowLayers[instance];
	if ((snow.flags.x & 1u) == 0u || snow.flags.y == 0u)
		return false;
	float3 projectionDirection = snow.directionAndSpace.xyz;
	if (snow.directionAndSpace.w == 1.0f)
		projectionDirection = mul((float3x3)objectToWorld, projectionDirection);
	if (!all(isfinite(projectionDirection)) || dot(projectionDirection, projectionDirection) < 1e-8f)
		return false;
	projectionDirection = normalize(projectionDirection);
	float3 edge0 = mul((float3x3)objectToWorld, hitSurface.position1 - hitSurface.position0);
	float3 edge1 = mul((float3x3)objectToWorld, hitSurface.position2 - hitSurface.position0);
	float3 faceNormal = cross(edge0, edge1);
	if (!all(isfinite(faceNormal)) || dot(faceNormal, faceNormal) < 1e-8f)
		return false;
	faceNormal = normalize(faceNormal);
	float3 normal = material.normal;
	if (!all(isfinite(normal)) || dot(normal, normal) < 1e-8f)
		return false;
	normal = normalize(normal);
	float3 projectionNormal = surface.geometricNormal;
	if (!all(isfinite(projectionNormal)) || dot(projectionNormal, projectionNormal) < 1e-8f)
		return false;
	projectionNormal = normalize(projectionNormal);
	float3 weights = pow(abs(projectionNormal), 6.0f) * step(0.4f, abs(faceNormal));
	float weightSum = weights.x + weights.y + weights.z;
	if (weightSum < 1e-6f) {
		weights = abs(projectionNormal);
		weightSum = weights.x + weights.y + weights.z;
	}
	weights /= max(weightSum, 1e-6f);
	float3 localPosition = hitSurface.position0 * (1.0f - barycentric.x - barycentric.y) +
		hitSurface.position1 * barycentric.x + hitSurface.position2 * barycentric.y;
	float3 worldPosition = mul(objectToWorld, float4(localPosition, 1.0f));
	if (!all(isfinite(worldPosition)))
		return false;
	float4 noise;
	if (!SampleNativeSnowTriplanar(snow.textures[1].zw, (snow.flags.w >> 6u) & 3u,
		worldPosition, weights, snow.projectedUVParams.z, footprint, noise))
		return false;
	float vertexAlpha = (flags & NativePBR::DrawVertexColorFlag) != 0u ? saturate(hitSurface.color.a) : 1.0f;
	float coverage = -snow.projectedUVParams.x * noise.x +
		(dot(normal, projectionDirection) * vertexAlpha - snow.projectedUVParams.w);
	if (!isfinite(coverage))
		return false;
	if (snow.flags.y == 1u) {
		if (coverage > 0.0f)
			material.baseColor = NativePBR::SrgbToLinear(max(snow.projectedUVParams2.xyz, 0.0f));
		return coverage > 0.0f;
	}
	if (snow.flags.y != 2u)
		return false;
	float diffuseNormalScale = snow.projectedUVParams3.x * snow.projectedUVParams.z;
	float detailNormalScale = snow.projectedUVParams3.y * snow.projectedUVParams.z;
	float4 diffuse, mappedNormal, detailNormal;
	if (!SampleNativeSnowTriplanar(snow.textures[0].xy, snow.flags.w & 3u,
		worldPosition, weights, diffuseNormalScale, footprint, diffuse) ||
		!SampleNativeSnowTriplanar(snow.textures[0].zw, (snow.flags.w >> 2u) & 3u,
			worldPosition, weights, diffuseNormalScale, footprint, mappedNormal) ||
		!SampleNativeSnowTriplanar(snow.textures[1].xy, (snow.flags.w >> 4u) & 3u,
			worldPosition, weights, detailNormalScale, footprint, detailNormal))
		return false;
	float blend = smoothstep(0.0f, 1.0f, 5.0f * (0.1f + coverage));
	if (blend <= 0.0f)
		return false;
	float3 authoredTint = snow.projectedUVParams2.xyz;
	if (pbrColorScale) {
		if (dot(snow.materialRGBScale.xyz, snow.materialRGBScale.xyz) < 1e-8f)
			return false;
		authoredTint *= snow.materialRGBScale.xyz;
	}
	float3 tint = NativePBR::SrgbToLinear(max(authoredTint, 0.0f));
	float3 diffuseLinear = snow.textures[0].x >= 256u ? diffuse.rgb : NativePBR::SrgbToLinear(diffuse.rgb);
	float3 projectedColor = max(diffuseLinear * tint, 0.0f);
	float3 primaryNormal = mappedNormal.xyz * 2.0f - 1.0f;
	float3 secondaryNormal = detailNormal.xyz * 2.0f - 1.0f;
	float3 tangentNormal = float3(primaryNormal.xy + secondaryNormal.xy, primaryNormal.z * secondaryNormal.z);
	if (!all(isfinite(tangentNormal)) || dot(tangentNormal, tangentNormal) < 1e-8f)
		return false;
	tangentNormal = normalize(tangentNormal);
	float3 projectedNormal = NativePBR::DecodeNormal(surface, tangentNormal * 0.5f + 0.5f);
	float3 finalNormal = lerp(material.normal, projectedNormal, blend);
	if (!all(isfinite(finalNormal)) || dot(finalNormal, finalNormal) < 1e-8f)
		return false;
	material.baseColor = lerp(material.baseColor, projectedColor, blend);
	material.normal = normalize(finalNormal);
	return true;
}

#endif

#include "Common/Color.hlsli"
#include "Common/GBuffer.hlsli"
#include "RedstoneGI/common.hlsli"
Texture2D<float> srcWorkingDepth : register(t0);
Texture2D<float4> srcNormalRoughness : register(t1);
Texture2D<float3> srcRadiance : register(t2);
Texture2D<float2> srcNoise : register(t3);
Texture2D<float3> srcDiffuseAlbedo : register(t5);
Texture2D<float3> srcSpecularAlbedo : register(t6);
RWTexture2D<float4> outSpecular : register(u0);
RWTexture2D<float4> outDiffuse : register(u1);
#include "RedstoneGI/Sampling.hlsli"

float4 EstimateDiffuse(Surface surface)
{
	if (DiffuseSlices == 0) return float4(0,0,0,-1);
	float3 radiance = 0;
	float hitDistance = 0;
	float hitWeight = 0;
	uint steps = AdaptiveSteps(surface, DiffuseSteps);
	for (uint i = 0; i < DiffuseSlices; ++i) {
	 Slice slice = BuildSlice(surface, i, DiffuseSlices);
	 uint visibility = 0;
	 [unroll] for (int side = -1; side <= 1; side += 2) {
	  for (uint j = 0; j < steps; ++j) {
	   float offset = SampleOffset(surface, i, j, steps);
	   SurfaceSample surfaceSample;
	   if (offset >= GIRadius || !LoadSurfaceSample(surface, slice, side, offset, surfaceSample)) continue;
	   uint mask = CoverageMask((surfaceSample.giAngles + slice.normalAngle) * Math::INV_PI + 0.5);
	   uint visible = mask & ~visibility;
	   visibility |= mask;
	   float weight = DiffuseCoverage(visible, slice);
	   if (weight <= 0) continue;
	   radiance += SampleEmission(surfaceSample) * weight * DistanceCompensation(offset * EffectRadius);
	   hitDistance += surfaceSample.distance * weight;
	   hitWeight += weight;
	  }
	 }
	}
	radiance *= (1.0 - GetDepthFade(surface.depth)) / DiffuseSlices;
	radiance = lerp(Color::RGBToLuminance(radiance).xxx, radiance, GISaturation);
	return float4(max(radiance, 0), hitWeight > 1e-6 ? hitDistance / hitWeight : -1.0);
}
#include "RedstoneGI/SpecularWeights.hlsli"
float4 EstimateSpecular(Surface surface, float roughness, bool traceRadiance)
{
	float3 radiance = 0;
	float hitDistance = 0;
	float hitWeight = 0;
	float lobeWeight = 0;
	if (!traceRadiance || SpecularSlices == 0 || dot(surface.normal, surface.view) <= 0) return float4(0,0,0,-1);
	uint steps = AdaptiveSteps(surface, SpecularSteps);
	for (uint i = 0; i < SpecularSlices; ++i) {
		Slice slice = BuildSlice(surface, i, SpecularSlices);
		float sectorWeights[32];
		[loop] for (uint sector = 0; sector < 32; ++sector) {
			sectorWeights[sector] = SpecularSectorWeight(surface, roughness, slice, sector);
			lobeWeight += sectorWeights[sector];
		}
		uint visibility = 0;
		[unroll] for (int side = -1; side <= 1; side += 2) {
			for (uint j = 0; j < steps; ++j) {
				float offset = SampleOffset(surface, i, j, steps);
				SurfaceSample surfaceSample;
				if (offset >= GIRadius || !LoadSurfaceSample(surface, slice, side, offset, surfaceSample)) continue;
				uint mask = CoverageMask((surfaceSample.giAngles + slice.normalAngle) * Math::INV_PI + 0.5);
				uint visible = mask & ~visibility;
				visibility |= mask;
				float weight = 0;
				while (visible) {
					uint sector = firstbitlow(visible);
					weight += sectorWeights[sector];
					visible &= visible - 1u;
				}
				if (weight <= 0) continue;
				radiance += SampleEmission(surfaceSample) * weight * DistanceCompensation(offset * EffectRadius);
				hitDistance += surfaceSample.distance * weight;
				hitWeight += weight;
			}
		}
	}
	float fade = 1.0 - GetDepthFade(surface.depth);
	return float4(lobeWeight > 0 ? max(0, radiance * fade / lobeWeight) : 0, hitWeight > 0 ? hitDistance / hitWeight : -1.0);
}
[numthreads(8,8,1)] void main(uint2 pixel : SV_DispatchThreadID)
{
	if (any(pixel >= uint2(OUT_FRAME_DIM))) return;
	if (RRCheckerboardEnabled && ((pixel.x + pixel.y) & 1u) != RRCheckerboardOrigin) return;
	uint2 surfacePixel = SelectSurfacePixel(srcWorkingDepth, pixel);
	float depth = srcWorkingDepth.Load(int3(surfacePixel, 0));
	float4 diffuse = float4(0,0,0,-1);
	float4 specular = float4(0,0,0,-1);
	float4 nr = srcNormalRoughness.Load(int3(surfacePixel, 0));
	if (isfinite(depth) && depth > FP_Z && depth < DepthFadeRange.y) {
#ifdef REVERSE_Z
	 depth *= 0.99999;
#else
	 depth *= 0.99920;
#endif
	 Surface surface = BuildSurface(pixel, depth, GBuffer::DecodeNormal(nr.xy));
	 specular = EstimateSpecular(surface, max(SpecularRoughnessFloor, saturate(1.0 - nr.z)), true);
	 diffuse = EstimateDiffuse(surface);
	}
	{
	 float3 diffuseFactor = Color::IrradianceToLinear(srcDiffuseAlbedo.Load(int3(surfacePixel, 0)) / Color::PBRLightingScale);
	 float3 specularFactor = srcSpecularAlbedo.Load(int3(surfacePixel, 0));
	 diffuse.rgb *= diffuseFactor;
	 specular.rgb *= specularFactor;
	 uint2 signalPixel = uint2(RRCheckerboardEnabled ? pixel.x >> 1u : pixel.x, pixel.y);
	 outDiffuse[signalPixel] = all(isfinite(diffuse)) ? diffuse : float4(0,0,0,-1);
	 outSpecular[signalPixel] = all(isfinite(specular)) ? specular : float4(0,0,0,-1);
	}
}

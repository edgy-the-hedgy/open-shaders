#include "Common/Color.hlsli"
#include "Common/GBuffer.hlsli"
#include "Upscaling/redstone/RedstoneGI/common.hlsli"
#include "Upscaling/redstone/RedstoneGI/OcclusionSettings.hlsli"
Texture2D<float> srcWorkingDepth : register(t0);
Texture2D<float4> srcNormalRoughness : register(t1);
Texture2D<float3> srcRadiance : register(t2);
Texture2D<float2> srcNoise : register(t3);
Texture2D<float2> srcNormal : register(t4);
RWTexture2D<unorm float> DiffuseOutput : register(u0);
RWTexture2D<unorm float> SpecularOutput : register(u1);
#include "Upscaling/redstone/RedstoneGI/Sampling.hlsli"
#include "Upscaling/redstone/RedstoneGI/SpecularWeights.hlsli"

uint OccludedSectors(Surface surface, Slice slice, uint steps, float radius, uint sliceIndex)
{
 uint covered = 0;
 [unroll] for (int side = -1; side <= 1; side += 2) {
  for (uint j = 0; j < steps; ++j) {
   float offset = SampleOffset(surface, sliceIndex, j, steps);
   SurfaceSample sample;
   if (offset >= 1 || !LoadSurfaceSample(surface, slice, side, offset, sample) || sample.distance > radius) continue;
   covered |= CoverageMask((sample.giAngles + slice.normalAngle) * Math::INV_PI + 0.5);
  }
 }
 return covered;
}
float EstimateOcclusion(Surface surface, float roughness, bool specular)
{
 uint slices = specular ? SpecularOcclusionSlices : DiffuseOcclusionSlices;
 uint steps = AdaptiveSteps(surface, specular ? SpecularOcclusionSteps : DiffuseOcclusionSteps);
 float radius = specular ? SpecularOcclusionRadius : DiffuseOcclusionRadius;
 float viewPixelWidth = surface.depth * abs(NDCToViewMul.x) * RCP_OUT_FRAME_DIM.x;
 surface.screenRadius = max(1.3, radius / max(viewPixelWidth, 1e-6));
 surface.minimumSample = 1.3 / surface.screenRadius;
 float blocked = 0, total = 0;
 for (uint i = 0; i < slices; ++i) {
  Slice slice = BuildSlice(surface, i, slices);
  uint covered = OccludedSectors(surface, slice, steps, radius, i);
  if (specular) {
   [loop] for (uint sector = 0; sector < 32; ++sector) {
    float weight = SpecularSectorWeight(surface, roughness, slice, sector);
    total += weight;
    if (covered & (1u << sector)) blocked += weight;
   }
  } else {
   total += DiffuseCoverage(0xffffffffu, slice);
   blocked += DiffuseCoverage(covered, slice);
  }
 }
 float visibility = total > 0 ? saturate(1 - blocked / total) : 1;
 return lerp(visibility, 1.0, GetDepthFade(surface.depth));
}
[numthreads(8,8,1)] void main(uint2 pixel : SV_DispatchThreadID)
{
 if (any(pixel >= uint2(OUT_FRAME_DIM))) return;
 if (RRCheckerboardEnabled && ((pixel.x + pixel.y) & 1u) != RRCheckerboardOrigin) return;
 float diffuse = 1, specular = 1;
 float depth = READ_DEPTH(srcWorkingDepth, pixel);
 if ((DiffuseOcclusionEnabled || SpecularOcclusionEnabled) && isfinite(depth) && depth > FP_Z && depth < DepthFadeRange.y) {
  float2 uv = (pixel + 0.5) * RCP_OUT_FRAME_DIM;
  float2 frameScale = FrameDim * RcpTexDim;
	uint2 surfacePixel = SelectSurfacePixel(srcWorkingDepth, pixel);
  float4 nr = srcNormalRoughness.Load(int3(surfacePixel, 0));
  Surface surface = BuildSurface(pixel, depth, GBuffer::DecodeNormal(nr.xy));
  if (DiffuseOcclusionEnabled) diffuse = EstimateOcclusion(surface, 1, false);
  if (SpecularOcclusionEnabled) specular = EstimateOcclusion(surface, max(SpecularRoughnessFloor, saturate(1 - nr.z)), true);
 }
 uint2 packed = uint2(RRCheckerboardEnabled ? pixel.x >> 1u : pixel.x, pixel.y);
 DiffuseOutput[packed] = isfinite(diffuse) ? saturate(diffuse) : 1;
 SpecularOutput[packed] = isfinite(specular) ? saturate(specular) : 1;
}

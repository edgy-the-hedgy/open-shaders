#include "Common/GBuffer.hlsli"
#include "RedstoneGI/common.hlsli"
#include "RedstoneGI/OcclusionSettings.hlsli"
Texture2D<float> DenoisedDiffuse : register(t0);
Texture2D<float> DenoisedSpecular : register(t1);
Texture2D<float> PackedDiffuse : register(t2);
Texture2D<float> PackedSpecular : register(t3);
Texture2D<float> LinearDepth : register(t4);
Texture2D<float4> NormalRoughness : register(t5);
RWTexture2D<unorm float2> Occlusion : register(u0);
float2 ResolveVisibility(uint2 pixel)
{
 if (OcclusionUseDenoised) return float2(DiffuseOcclusionEnabled ? DenoisedDiffuse[pixel] : 1, SpecularOcclusionEnabled ? DenoisedSpecular[pixel] : 1);
 if (!RRCheckerboardEnabled || ((pixel.x + pixel.y) & 1u) == RRCheckerboardOrigin) {
  uint2 packed = uint2(RRCheckerboardEnabled ? pixel.x >> 1u : pixel.x, pixel.y);
  return float2(PackedDiffuse[packed], PackedSpecular[packed]);
 }
 const int2 offsets[4] = {int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)};
 float depth = LinearDepth[pixel];
 float4 guide = NormalRoughness[pixel];
 float3 normal = GBuffer::DecodeNormal(guide.xy);
 float2 sum = 0, weights = 0;
 [unroll] for (uint i = 0; i < 4; ++i) {
  int2 p = int2(pixel) + offsets[i];
  if (any(p < 0) || any(p >= int2(OUT_FRAME_DIM))) continue;
  float4 other = NormalRoughness[p];
  float depthWeight = exp2(-64.0 * abs(LinearDepth[p] - depth) / max(abs(depth), 0.001));
  float normalWeight = pow(saturate(dot(normal, GBuffer::DecodeNormal(other.xy))), 32.0);
  float diffuseWeight = depthWeight * normalWeight;
  float2 weight = float2(diffuseWeight, diffuseWeight * exp2(-8.0 * abs(guide.z - other.z)));
  uint2 packed = uint2(p.x >> 1u, p.y);
  sum += float2(PackedDiffuse[packed], PackedSpecular[packed]) * weight;
  weights += weight;
 }
 return float2(weights.x > 1e-6 ? sum.x / weights.x : 1, weights.y > 1e-6 ? sum.y / weights.y : 1);
}
[numthreads(8,8,1)] void main(uint2 pixel : SV_DispatchThreadID)
{
 if (any(pixel >= uint2(OUT_FRAME_DIM))) return;
 float2 visibility = ResolveVisibility(pixel);
 visibility = float2(DiffuseOcclusionEnabled ? visibility.x : 1, SpecularOcclusionEnabled ? visibility.y : 1);
 Occlusion[pixel] = all(isfinite(visibility)) ? saturate(visibility) : 1;
}

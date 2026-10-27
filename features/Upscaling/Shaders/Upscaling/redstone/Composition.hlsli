#ifndef REDSTONE_COMPOSITION
#define REDSTONE_COMPOSITION

Texture2D<float4> RedstoneSpecular : register(t20);
Texture2D<float4> RedstoneDiffuse : register(t21);
Texture2D<float> RedstoneLinearDepth : register(t22);
Texture2D<float4> RedstoneNormalRoughness : register(t23);
Texture2D<float> RedstoneFullDepth : register(t24);
Texture2D<float2> RedstoneOcclusion : register(t25);

cbuffer RedstoneCompositionSettings : register(b13)
{
	uint2 RedstoneInputSize;
	uint2 RedstoneFrameSize;
	float RedstoneDiffuseScale;
	float RedstoneSpecularScale;
	uint RedstoneLightingReady;
	uint RedstoneOcclusionMask;
	float RedstoneRoughnessFloor;
	float RedstoneDiffuseOcclusionStrength;
	float RedstoneSpecularOcclusionStrength;
	uint RedstoneCompositionPaddingTail;
};

struct RedstoneReconstruction
{
 float3 diffuse;
 float3 specular;
 float2 occlusion;
 float2 weights;
};
void AccumulateRedstone(int2 samplePx, float spatialWeight, float depth, float3 normalWS, float roughness, float2 enabled, inout RedstoneReconstruction result)
{
 if (any(samplePx < 0) || any(samplePx >= int2(RedstoneInputSize))) return;
 const float depthSharpness = 128.0;
 const float normalSharpness = 32.0;
 const float roughnessSharpness = 8.0;
 const float maximumRelativeDepthDifference = 0.02;
 const float minimumNormalAlignment = 0.5;
 float sampleDepth = RedstoneLinearDepth[samplePx];
 float4 guide = RedstoneNormalRoughness[samplePx];
 float3 sampleNormal = -GBuffer::DecodeNormal(guide.xy);
 float relativeDepthDifference = abs(sampleDepth - depth) / max(abs(depth), 0.001);
 float normalAlignment = dot(sampleNormal, normalWS);
 if (!isfinite(sampleDepth) || sampleDepth <= 0 || relativeDepthDifference > maximumRelativeDepthDifference || normalAlignment < minimumNormalAlignment) return;
 float diffuseWeight = spatialWeight * exp2(-depthSharpness * relativeDepthDifference) * pow(saturate(normalAlignment), normalSharpness);
 float specularWeight = diffuseWeight * exp2(-roughnessSharpness * abs(guide.z - max(RedstoneRoughnessFloor, roughness)));
 float2 weights = float2(diffuseWeight, specularWeight) * enabled;
 result.diffuse += RedstoneDiffuse[samplePx].rgb * weights.x;
 result.specular += RedstoneSpecular[samplePx].rgb * weights.y;
 if (RedstoneOcclusionMask) result.occlusion += RedstoneOcclusion[samplePx] * weights;
 result.weights += weights;
}

/** @brief Reconstructs all four signals at the engine pixel using matching current-frame surface guides. */
void SampleRedstone(uint2 pixel, float3 normalWS, float roughness, out float3 diffuse, out float3 specular, out float2 visibility)
{
 diffuse = 0;
 specular = 0;
 visibility = 1;
 if (!RedstoneLightingReady || any(RedstoneInputSize == 0) || any(RedstoneFrameSize == 0)) return;
 float screenDepth = RedstoneFullDepth[pixel];
 if (screenDepth == FrameBuffer::FarPlaneDepth()) return;
 float standardDepth = FrameBuffer::ToStandardDepth(screenDepth);
 float depth = SharedData::CameraData.w / (-standardDepth * SharedData::CameraData.z + SharedData::CameraData.x);
 if (!isfinite(depth) || depth <= 0) return;
 float2 position = (float2(pixel) + 0.5) * float2(RedstoneInputSize) / float2(RedstoneFrameSize) - 0.5;
 int2 base = int2(floor(position));
 float2 fraction = frac(position);
 RedstoneReconstruction result = (RedstoneReconstruction)0;
 [unroll] for (int y = 0; y < 2; ++y) {
  [unroll] for (int x = 0; x < 2; ++x) {
   int2 samplePx = clamp(base + int2(x,y), 0, int2(RedstoneInputSize) - 1);
   float weight = (x ? fraction.x : 1 - fraction.x) * (y ? fraction.y : 1 - fraction.y);
   AccumulateRedstone(samplePx, weight, depth, normalWS, roughness, 1, result);
  }
 }
 float2 missing = float2(result.weights.x <= 1e-6, result.weights.y <= 1e-6);
 if (any(missing) && any(RedstoneInputSize != RedstoneFrameSize)) {
  int2 nearest = int2(floor(position + 0.5));
  [loop] for (int y = -2; y <= 2; ++y) {
   [loop] for (int x = -2; x <= 2; ++x) {
    int2 samplePx = nearest + int2(x,y);
    float2 distance = float2(samplePx) - position;
    AccumulateRedstone(samplePx, exp2(-dot(distance,distance)), depth, normalWS, roughness, missing, result);
   }
  }
 }
 diffuse = result.weights.x > 1e-6 ? result.diffuse / result.weights.x * RedstoneDiffuseScale : 0;
 specular = result.weights.y > 1e-6 ? result.specular / result.weights.y * RedstoneSpecularScale : 0;
 visibility = float2(result.weights.x > 1e-6 ? result.occlusion.x / result.weights.x : 1, result.weights.y > 1e-6 ? result.occlusion.y / result.weights.y : 1);
 visibility = float2((RedstoneOcclusionMask & 1u) ? lerp(1.0, saturate(visibility.x), RedstoneDiffuseOcclusionStrength) : 1,
  (RedstoneOcclusionMask & 2u) ? lerp(1.0, saturate(visibility.y), RedstoneSpecularOcclusionStrength) : 1);
 visibility = all(isfinite(visibility)) ? visibility : 1;
 diffuse = all(isfinite(diffuse)) ? max(diffuse,0) : 0;
 specular = all(isfinite(specular)) ? max(specular,0) : 0;
}
#endif

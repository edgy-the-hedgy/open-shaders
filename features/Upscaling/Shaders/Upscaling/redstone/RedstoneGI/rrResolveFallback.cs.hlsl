#include "Common/GBuffer.hlsli"
#include "Upscaling/redstone/RedstoneGI/common.hlsli"

Texture2D<float4> PackedSpecular : register(t0);
Texture2D<float4> PackedDiffuse : register(t1);
Texture2D<float> LinearDepth : register(t2);
Texture2D<float4> NormalRoughness : register(t3);
RWTexture2D<float4> SpecularOutput : register(u0);
RWTexture2D<float4> DiffuseOutput : register(u1);

/** @brief Resolves the current paired signals when the RR provider cannot process this frame. */
[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID)
{
	uint2 size = uint2(OUT_FRAME_DIM);
	if (any(dtid.xy >= size))
		return;
	if (!RRCheckerboardEnabled || ((dtid.x + dtid.y) & 1u) == RRCheckerboardOrigin) {
		uint2 signalPx = uint2(RRCheckerboardEnabled ? dtid.x >> 1u : dtid.x, dtid.y);
		SpecularOutput[dtid.xy] = PackedSpecular[signalPx];
		DiffuseOutput[dtid.xy] = PackedDiffuse[signalPx];
		return;
	}
	const float depthSharpness = 64.0;
	const float normalSharpness = 32.0;
	float depth = LinearDepth[dtid.xy];
	float3 normal = GBuffer::DecodeNormal(NormalRoughness[dtid.xy].xy);
	float3 specular = 0;
	float3 diffuse = 0;
	float weightSum = 0;
	const int2 offsets[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
	[unroll] for (uint i = 0; i < 4; ++i) {
		int2 samplePx = int2(dtid.xy) + offsets[i];
		if (any(samplePx < 0) || any(samplePx >= int2(size)))
			continue;
		float sampleDepth = LinearDepth[samplePx];
		float3 sampleNormal = GBuffer::DecodeNormal(NormalRoughness[samplePx].xy);
		float weight = exp2(-depthSharpness * abs(sampleDepth - depth) / max(abs(depth), 0.001));
		weight *= pow(saturate(dot(sampleNormal, normal)), normalSharpness);
		uint2 signalPx = uint2(samplePx.x >> 1, samplePx.y);
		specular += PackedSpecular[signalPx].rgb * weight;
		diffuse += PackedDiffuse[signalPx].rgb * weight;
		weightSum += weight;
	}
	SpecularOutput[dtid.xy] = float4(weightSum > 1e-6 ? specular / weightSum : 0, -1);
	DiffuseOutput[dtid.xy] = float4(weightSum > 1e-6 ? diffuse / weightSum : 0, -1);
}

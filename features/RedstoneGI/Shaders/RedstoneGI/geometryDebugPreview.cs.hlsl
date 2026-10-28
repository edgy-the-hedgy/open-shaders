#include "Common/ReverseZ.hlsli"

Texture2D<float> workingDepth : register(t0);
Texture2D<float> sceneDepth : register(t1);
StructuredBuffer<uint> maximumDepth : register(t2);
RWTexture2D<float4> preview : register(u0);
RWStructuredBuffer<uint> reducedMaximumDepth : register(u1);

cbuffer GeometryPreviewSettings : register(b0)
{
	uint2 inputSize;
	uint2 padding;
};

bool ValidDepth(uint2 pixel, float depth)
{
	float rawDepth = sceneDepth.Load(int3(pixel, 0));
	return isfinite(depth) && depth > 0 && isfinite(rawDepth) && FrameBuffer::IsNearerDepth(rawDepth, FrameBuffer::FarPlaneDepth());
}

groupshared uint groupMaximum[256];

[numthreads(16, 16, 1)]
void ReduceDepth(uint2 pixel : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
	uint value = 0;
	if (all(pixel < inputSize)) {
		float depth = workingDepth.Load(int3(pixel, 0));
		if (ValidDepth(pixel, depth)) value = asuint(depth);
	}
	groupMaximum[index] = value;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 128; stride > 0; stride >>= 1) {
		if (index < stride) groupMaximum[index] = max(groupMaximum[index], groupMaximum[index + stride]);
		GroupMemoryBarrierWithGroupSync();
	}
	if (index == 0) InterlockedMax(reducedMaximumDepth[0], groupMaximum[0]);
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
	if (any(pixel >= inputSize)) return;
	float depth = workingDepth.Load(int3(pixel, 0));
	float farthest = asfloat(maximumDepth[0]);
	if (!ValidDepth(pixel, depth) || farthest <= 0) {
		preview[pixel] = float4(1, 0, 0, 1);
		return;
	}
	float value = 1 - saturate(depth / farthest);
	preview[pixel] = float4(value.xxx, 1);
}

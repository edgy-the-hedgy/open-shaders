#ifndef REDSTONE_GI_COMMON
#define REDSTONE_GI_COMMON
#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/VR.hlsli"

cbuffer RedstoneGISettings : register(b1)
{
	float2 NDCToViewMul;
	float2 NDCToViewAdd;
	float2 TexDim;
	float2 RcpTexDim;
	float2 FrameDim;
	float2 RcpFrameDim;
	uint FrameIndex;
	uint DiffuseSlices;
	uint DiffuseSteps;
	uint SpecularSlices;
	uint SpecularSteps;
	uint RRHistoryValid;
	uint RRCheckerboardEnabled;
	uint RRCheckerboardOrigin;
	float MinScreenRadius;
	float GIRadius;
	float EffectRadius;
	float SpecularRoughnessFloor;
	float2 DepthFadeRange;
	float GISaturation;
	float GIDistanceCompensation;
	float GICompensationMaxDist;
	float GIStrength;
	float DepthFadeScaleConst;
	float OccluderDepth;
	row_major float4x4 PreviousCameraView;
	row_major float4x4 PreviousCameraProjection;
	float4 PreviousCameraOrigin;
	uint GeometryHistoryValid;
	uint3 geometryPadding;
};
SamplerState samplerPointClamp : register(s0);
SamplerState samplerLinearClamp : register(s1);
#define FP_Z 18.0
#ifdef HALF_RES
	#define RES_MIP 1
	#define READ_DEPTH(tex, px) tex.Load(int3(SelectSurfacePixel(tex, px), 0))
	#define FULLRES_LOAD(tex, px, uv, samp) tex.SampleLevel(samp, uv, 0)
	#define OUT_FRAME_DIM float2(uint2(FrameDim) >> 1u)
	#define RCP_OUT_FRAME_DIM (1.0 / OUT_FRAME_DIM)
	#define OUT_FRAME_SCALE (frameScale * 0.5)
#elif defined(QUARTER_RES)
	#define RES_MIP 2
	#define READ_DEPTH(tex, px) tex.Load(int3(SelectSurfacePixel(tex, px), 0))
	#define FULLRES_LOAD(tex, px, uv, samp) tex.SampleLevel(samp, uv, 0)
	#define OUT_FRAME_DIM float2(uint2(FrameDim) >> 2u)
	#define RCP_OUT_FRAME_DIM (1.0 / OUT_FRAME_DIM)
	#define OUT_FRAME_SCALE (frameScale * 0.25)
#else
	#define RES_MIP 0
	#define READ_DEPTH(tex, px) tex[px]
	#define FULLRES_LOAD(tex, px, uv, samp) tex[px]
	#define OUT_FRAME_DIM FrameDim
	#define RCP_OUT_FRAME_DIM RcpFrameDim
	#define OUT_FRAME_SCALE frameScale
#endif
uint2 SelectSurfacePixel(Texture2D<float> depthTexture, uint2 pixel)
{
	return min(uint2((float2(pixel) + 0.5) * FrameDim / OUT_FRAME_DIM), uint2(FrameDim) - 1u);
}
float3 ScreenToViewPosition(float2 screen, float depth, uint eyeIndex = 0)
{
	float4 position = mul(FrameBuffer::CameraProjInverse[eyeIndex], float4(screen * float2(2, -2) + float2(-1, 1), 0.5, 1));
	return position.xyz * (depth / position.z);
}
float ScreenToViewDepth(float screenDepth)
{
	float depth = FrameBuffer::ToStandardDepth(screenDepth);
	return SharedData::CameraData.w / (-depth * SharedData::CameraData.z + SharedData::CameraData.x);
}
float3 ViewToWorldVector(float3 direction, float4x4 inverseView)
{
	return mul((float3x3)inverseView, direction);
}
float GetDepthFade(float depth)
{
	return saturate((depth - DepthFadeRange.x) * DepthFadeScaleConst);
}
#endif

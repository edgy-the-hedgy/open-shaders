#ifndef NATIVE_PBR_OPAQUE_INPUTS_PS_HLSLI
#define NATIVE_PBR_OPAQUE_INPUTS_PS_HLSLI

#include "Common/GBuffer.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/VR.hlsli"

cbuffer PerTechnique : register(b0)
{
	float4 VPOSOffset : packoffset(c2);
};

struct Input
{
	float4 Position : SV_POSITION0;
	float2 TexCoord0 : TEXCOORD0;
	float3 TBN0 : TEXCOORD1;
	float3 TBN1 : TEXCOORD2;
	float3 TBN2 : TEXCOORD3;
	float4 WorldPosition : POSITION1;
	float4 PreviousWorldPosition : POSITION2;
	float4 Color : COLOR0;
};

struct Output
{
	float4 Diffuse : SV_Target0;
	float4 MotionVectors : SV_Target1;
	float4 NormalGlossiness : SV_Target2;
	float4 Albedo : SV_Target3;
	float4 Specular : SV_Target4;
	float4 Reflectance : SV_Target5;
	float4 Masks : SV_Target6;
	float4 Masks2 : SV_Target7;
};

Output main(Input input)
{
	const uint eyeIndex = Stereo::GetEyeIndexPS(input.Position, VPOSOffset);
	const float2 motion = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition, eyeIndex);
	const float3 geometricNormal = normalize(FrameBuffer::WorldToView(normalize(input.TBN2), false, eyeIndex));

	Output output;
	output.Diffuse = float4(0, 0, 0, 1);
	output.MotionVectors = float4(motion, 0, 1);
	output.NormalGlossiness = float4(GBuffer::EncodeNormal(geometricNormal), 0, 1);
	output.Albedo = float4(input.Color.rgb, 1);
	output.Specular = float4(0, 0, 0, 1);
	output.Reflectance = float4(0, 0, 0, 1);
	output.Masks = float4(0, 0, 0, 1);
	output.Masks2 = float4(0, 0, 0, 1);
	return output;
}

#endif

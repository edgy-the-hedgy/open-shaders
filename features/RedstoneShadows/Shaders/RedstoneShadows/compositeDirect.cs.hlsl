#ifndef CSHADER
#define CSHADER
#endif
#include "Common/Color.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/ReverseZ.hlsli"
#include "RedstoneShadows/directContract.hlsli"
#include "RedstoneGI/Composition.hlsli"

Texture2D<float4> DirectLighting : register(t0);
Texture2D<float4> NativeNormalRoughness : register(t1);
Texture2D<float4> RawDirectDiffuse : register(t2);
Texture2D<float4> RawDirectSpecular : register(t3);
Texture2D<float4> FilteredDirectDiffuse : register(t4);
Texture2D<float4> FilteredDirectSpecular : register(t5);
Texture2D<float4> RawDirectSpecularAlbedo : register(t6);
Texture2D<float4> RawIndirectDiffuse : register(t7);
Texture2D<float4> RawIndirectSpecular : register(t8);
Texture2D<float4> FilteredIndirectDiffuse : register(t9);
Texture2D<float4> FilteredIndirectSpecular : register(t10);
Texture2D<float> RRDepth : register(t11);
Texture2D<float4> RRNormalRoughness : register(t12);
Texture2D<float4> NativeReceivers : register(t13);
Texture2D<float4> RawNormalRoughness : register(t14);
RWTexture2D<float4> SceneColor : register(u0);
cbuffer LightingComposite : register(b0)
{
	uint UseIndirect;
	uint UseDirectRR;
	float IndirectStrength;
	uint UseDXRIndirect;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint width, height;
	DirectLighting.GetDimensions(width, height);
	if (id.x >= width || id.y >= height) return;
	uint targetWidth, targetHeight;
	SceneColor.GetDimensions(targetWidth, targetHeight);
	if (id.x >= targetWidth || id.y >= targetHeight) return;
	float4 lighting = DirectLighting.Load(int3(id.xy, 0));
	if (lighting.w != DirectRadianceTag || !all(isfinite(lighting.rgb))) return;
	uint rrFlags = UseDirectRR;
	uint2 rrPixel = 0;
	if (rrFlags != 0) {
		uint rrWidth, rrHeight; RRDepth.GetDimensions(rrWidth, rrHeight);
		rrPixel = min(uint2((float2(id.xy) + 0.5) * uint2(rrWidth, rrHeight) / uint2(width, height)), uint2(rrWidth, rrHeight) - 1);
		float depth = mul(FrameBuffer::CameraView[0], float4(NativeReceivers.Load(int3(id.xy, 0)).xyz, 1)).z;
		float4 guide = RRNormalRoughness.Load(int3(rrPixel, 0));
		float4 sourceGuide = RawNormalRoughness.Load(int3(id.xy, 0));
		if (abs(RRDepth.Load(int3(rrPixel, 0)) - depth) > max(0.5, abs(depth) * 0.02) ||
			dot(GBuffer::DecodeNormal(guide.xy), GBuffer::DecodeNormal(sourceGuide.xy)) < 0.9 || abs(guide.z - sourceGuide.z) > 0.1) rrFlags = 0;
	}
	if ((rrFlags & 1u) != 0) {
		float3 filteredDiffuse = FilteredDirectDiffuse.Load(int3(rrPixel, 0)).rgb;
		float3 filteredSpecular = FilteredDirectSpecular.Load(int3(rrPixel, 0)).rgb;
		if (all(isfinite(filteredDiffuse)) && all(isfinite(filteredSpecular)) && all(filteredDiffuse >= 0) && all(filteredSpecular >= 0))
			lighting.rgb = float3(RawDirectDiffuse.Load(int3(id.xy, 0)).a, RawDirectSpecular.Load(int3(id.xy, 0)).a, RawDirectSpecularAlbedo.Load(int3(id.xy, 0)).a) + filteredDiffuse + filteredSpecular;
	}
	if (UseDXRIndirect != 0) {
		float4 diffuse = RawIndirectDiffuse.Load(int3(id.xy, 0)), specular = RawIndirectSpecular.Load(int3(id.xy, 0));
		if (diffuse.a >= -1 && specular.a >= -1) {
			float3 d = (rrFlags & 2u) != 0 ? FilteredIndirectDiffuse.Load(int3(rrPixel, 0)).rgb : diffuse.rgb;
			float3 s = (rrFlags & 2u) != 0 ? FilteredIndirectSpecular.Load(int3(rrPixel, 0)).rgb : specular.rgb;
			d = all(isfinite(d)) && all(d >= 0) ? d : diffuse.rgb;
			s = all(isfinite(s)) && all(s >= 0) ? s : specular.rgb;
			lighting.rgb += (d + s) * IndirectStrength;
		}
	}
	float3 working = ENABLE_LL ? Color::GamutTransform(lighting.rgb) : lighting.rgb;
	if (UseIndirect != 0) {
		float4 nr = NativeNormalRoughness.Load(int3(id.xy, 0));
		float3 normal = normalize(mul((float3x3)FrameBuffer::CameraViewInverse[0], GBuffer::DecodeNormal(nr.xy)));
		float3 diffuse, specular;
		float2 visibility;
		SampleRedstone(id.xy, normal, 1 - nr.z, diffuse, specular, visibility);
		working += (diffuse + specular) * IndirectStrength;
	}
	if (!all(isfinite(working))) return;
	SceneColor[id.xy] = float4(Color::SceneLinearToGamma(working), 1);
}

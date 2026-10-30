#ifndef CSHADER
#define CSHADER
#endif
#include "Common/Color.hlsli"
#include "RedstoneShadows/directContract.hlsli"

Texture2D<float4> Lighting : register(t0);
Texture2D<float4> Receivers : register(t1);
Texture2D<float4> NativeMotion : register(t2);
RWTexture2D<float> LinearDepth : register(u0);
RWTexture2D<float4> Motion : register(u1);
cbuffer RRGrid : register(b0) { uint2 NativeSize; uint2 RRSize; uint Checkerboard; uint Phase; uint2 GridPadding; };

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint width, height;
	LinearDepth.GetDimensions(width, height);
	if (id.x >= width || id.y >= height) return;
	uint2 pixel = min(uint2((float2(id.xy) + 0.5) * NativeSize / RRSize), NativeSize - 1);
	float4 lighting = Lighting.Load(int3(pixel, 0));
	float3 relative = Receivers.Load(int3(pixel, 0)).xyz;
	float4 view = mul(FrameBuffer::CameraView[0], float4(relative, 1));
	bool valid = lighting.w == DirectRadianceTag && all(isfinite(view)) && view.z > 0;
	LinearDepth[id.xy] = valid ? view.z : 0;
	float2 motion = NativeMotion.Load(int3(pixel, 0)).xy;
	bool nativeMotionValid = all(isfinite(motion));
	motion = nativeMotionValid ? motion : 0;
	float3 previousRelative = relative + FrameBuffer::CameraPosAdjust[0].xyz - FrameBuffer::CameraPreviousPosAdjust[0].xyz;
	float4 previousClip = mul(FrameBuffer::CameraPreviousViewProjUnjittered[0], float4(previousRelative, 1));
	float4 currentClip = mul(FrameBuffer::CameraProjUnjittered[0], view);
	bool correspondence = false;
	if (valid && nativeMotionValid && all(isfinite(previousClip)) && all(isfinite(currentClip)) && previousClip.w > 0 && currentClip.w > 0) {
		float2 expected = (previousClip.xy / previousClip.w - currentClip.xy / currentClip.w) * float2(0.5, -0.5);
		correspondence = all(abs(expected - motion) * float2(NativeSize) <= 0.5);
	}
	float delta = correspondence ? previousClip.w - view.z : 0;
	static const float MaximumHalfDepthDelta = 65504;
	if (!isfinite(delta) || abs(delta) > MaximumHalfDepthDelta) { delta = 0; correspondence = false; }
	Motion[id.xy] = valid ? float4(motion, delta, correspondence ? 1 : 0) : 0;
}

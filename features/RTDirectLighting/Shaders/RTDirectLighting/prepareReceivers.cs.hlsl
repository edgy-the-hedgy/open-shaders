#include "Common/ReverseZ.hlsli"
#ifndef CSHADER
#define CSHADER
#endif
#include "Common/Color.hlsli"
#include "Common/ReceiverRoom.hlsli"
#include "RTDirectLighting/directContract.hlsli"

Texture2D<float> Depth : register(t0);
// t1 reserved; native light snapshot is uploaded directly to the shared texture.
Texture2D<float4> RoomMetadata : register(t2);
Texture2D<float4> MotionVectors : register(t3);
RWTexture2D<float4> Receivers : register(u0);
RWTexture2D<float4> LightSnapshot : register(u1);
RWTexture2D<float4> ShadowMotion : register(u2);
cbuffer Grid : register(b0)
{
	uint2 Size;
	uint2 NativeSize;
	uint LightReference;
	uint LightBase;
	uint RoomGeneration;
	uint HasRoomMetadata;
	float3 LightEyeOrigin;
	uint LightCount;
	uint RequestedLightCount;
	uint HasShadowMotion;
    uint2 RequestedPadding;
	uint4 RequestedIdentities[MaximumDirectLights];
	uint4 RoomReferences[128];
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.y == 0 && id.x < 128) {
		uint reference = RoomReferences[id.x].x;
		uint code = ReceiverRoom::Unknown, matches = 0;
		if (reference != 0 && RoomGeneration != 0) {
			for (uint room = 0; room < 128; ++room)
				if (RoomReferences[room].y == reference) { code = ReceiverRoom::FirstRoom + room; ++matches; }
		}
		LightSnapshot[uint2(id.x, 1)] = float4(matches == 1 ? code : ReceiverRoom::Unknown, RoomGeneration, 0, 0);
	}
	if (any(id.xy >= Size)) return;
	uint2 pixel = min(uint2((float2(id.xy) + 0.5) * NativeSize / Size), NativeSize - 1);
	float raw = Depth.Load(int3(pixel, 0));
	float depth = FrameBuffer::ToStandardDepth(raw);
	float2 uv = (float2(pixel) + 0.5) / NativeSize;
	float projectedDepth = FrameBuffer::IsReverseProjection() ? 1.0 - depth : depth;
	float4 view = mul(FrameBuffer::CameraProjInverse[0], float4(uv * float2(2, -2) + float2(-1, 1), projectedDepth, 1));
	bool valid = isfinite(raw) && raw != FrameBuffer::FarPlaneDepth() && depth >= 0 && depth < 1 && abs(view.w) > 1e-8;
	float3 relative = mul(FrameBuffer::CameraViewInverse[0], float4(view.xyz / view.w, 1)).xyz;
	valid = valid && all(isfinite(relative));
	uint room = HasRoomMetadata != 0 ? ReceiverRoom::Decode(RoomMetadata.Load(int3(pixel, 0)), RoomGeneration) : ReceiverRoom::Unknown;
	Receivers[id.xy] = valid ? float4(relative, float(room)) : 0;
    if (HasShadowMotion != 0u) ShadowMotion[id.xy] = MotionVectors.Load(int3(pixel, 0));
}

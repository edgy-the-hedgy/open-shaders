#include "Common/ReverseZ.hlsli"
#ifndef CSHADER
#define CSHADER
#endif
#include "Common/Color.hlsli"
#include "Common/ReceiverRoom.hlsli"
#include "RedstoneShadows/directContract.hlsli"

namespace LightLimitFix
{
#include "LightLimitFix/Common.hlsli"
}

Texture2D<float> Depth : register(t0);
StructuredBuffer<LightLimitFix::Light> Lights : register(t1);
Texture2D<float4> RoomMetadata : register(t2);
RWTexture2D<float4> Receivers : register(u0);
RWTexture2D<float4> LightSnapshot : register(u1);
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
	uint3 RequestedPadding;
	uint4 RequestedIdentities[MaximumDirectLights];
	uint4 RoomReferences[128];
};

void SnapshotLight(uint lightIndex)
{
	uint snapshotBase = lightIndex * LightSnapshotTexels;
	uint referenceID = RequestedIdentities[lightIndex].x;
	uint baseID = RequestedIdentities[lightIndex].y;
	for (uint texel = 0; texel < LightSnapshotTexels; ++texel) LightSnapshot[uint2(snapshotBase + texel, 0)] = 0;
	LightSnapshot[uint2(snapshotBase + 5, 0)] = float4(0, LightNoTable, 0, 0);
	if (LightCount == 0) return;
	uint count, stride;
	Lights.GetDimensions(count, stride);
	LightSnapshot[uint2(snapshotBase + 5, 0)] = float4(0, LightInvalidTable, 0, 0);
	if (LightCount > count || stride != 96) return;
	LightSnapshot[uint2(snapshotBase + 5, 0)] = float4(0, LightUnresolvedReference, 0, 0);
	if (referenceID == 0 || baseID == 0) return;
	for (uint prior = 0; prior < lightIndex; ++prior) {
		if (all(RequestedIdentities[prior].xy == uint2(referenceID, baseID))) {
			LightSnapshot[uint2(snapshotBase + 5, 0)] = float4(0, LightAmbiguous, 0, asfloat(referenceID));
			return;
		}
	}
	uint matches = 0;
	LightLimitFix::Light selected = (LightLimitFix::Light)0;
	for (uint index = 0; index < LightCount; ++index) {
		LightLimitFix::Light light = Lights[index];
		if (light.sourceReference != referenceID || light.sourceBase != baseID) continue;
		++matches;
		selected = light;
	}
	LightSnapshot[uint2(snapshotBase + 5, 0)] = float4(asfloat(selected.lightFlags), matches == 0 ? LightNoMatch : LightAmbiguous, matches, asfloat(referenceID));
	if (matches != 1) return;
	LightSnapshot[uint2(snapshotBase + 5, 0)] = float4(asfloat(selected.lightFlags), LightUnsupported, matches, asfloat(referenceID));
	static const uint Unsupported = LightLimitFix::LightFlags::Disabled | LightLimitFix::LightFlags::Spot | LightLimitFix::LightFlags::Particle;
	if ((selected.lightFlags & Unsupported) != 0 || (selected.lightFlags & LightLimitFix::LightFlags::OmniDirectional) == 0) return;
	LightSnapshot[uint2(snapshotBase + 5, 0)] = float4(asfloat(selected.lightFlags), LightInvalidParameters, matches, asfloat(referenceID));
	bool valid = all(isfinite(float4(selected.color, selected.fade))) && all(selected.color >= 0) && selected.fade > 0 &&
		all(isfinite(float4(selected.radius, selected.invRadius, selected.fadeZone, selected.sizeBias))) &&
		all(isfinite(selected.positionWS[0].xyz)) && all(isfinite(selected.positionWS[1].xyz)) &&
		selected.radius > 0 && selected.invRadius > 0 && selected.fadeZone >= 0 && selected.sizeBias >= 0 &&
		((selected.lightFlags & LightLimitFix::LightFlags::InverseSquare) == 0 || (selected.fadeZone > 0 && selected.sizeBias > 0));
	if (!valid) return;
	LightSnapshot[uint2(snapshotBase + 5, 0)] = float4(asfloat(selected.lightFlags), LightInvalidRadiance, matches, asfloat(referenceID));
	float3 radiance = Color::PointLight(selected.color, (selected.lightFlags & LightLimitFix::LightFlags::Linear) != 0, selected.lightFlags) * selected.fade;
	radiance = ENABLE_LL ? (ENABLE_ACEScg ? AP1TosRGB(radiance) : radiance) : Color::SkyrimGammaToLinear(radiance);
	if (!all(isfinite(radiance)) || any(radiance < 0)) return;
	LightSnapshot[uint2(snapshotBase + 0, 0)] = float4(selected.color, selected.fade);
	LightSnapshot[uint2(snapshotBase + 1, 0)] = float4(selected.radius, selected.invRadius, selected.fadeZone, selected.sizeBias);
	LightSnapshot[uint2(snapshotBase + 2, 0)] = selected.positionWS[0];
	LightSnapshot[uint2(snapshotBase + 3, 0)] = selected.positionWS[1];
	LightSnapshot[uint2(snapshotBase + 4, 0)] = asfloat(selected.roomFlags);
	LightSnapshot[uint2(snapshotBase + 5, 0)] = float4(asfloat(selected.lightFlags), LightReady, matches, asfloat(referenceID));
	LightSnapshot[uint2(snapshotBase + 6, 0)] = float4(LightEyeOrigin, 1);
	LightSnapshot[uint2(snapshotBase + 7, 0)] = float4(radiance, 1);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.y == 0 && id.x < RequestedLightCount && id.x < MaximumDirectLights) SnapshotLight(id.x);
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
}

#include "RedstoneShadows/directContract.hlsli"

Texture2D<float4> Shadow : register(t0);
RWTexture2D<float4> Preview : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint width, height;
	Preview.GetDimensions(width, height);
	if (id.x >= width || id.y >= height) return;
	float4 shadow = Shadow.Load(int3(id.xy, 0));
	if (shadow.w == 0) Preview[id.xy] = float4(0, 0, 0, 1);
	else if (shadow.w == UnavailableTag) {
		uint reason = uint(shadow.x);
		float3 color = reason == MissingReceiver ? float3(1, 0, 0) : reason == MissedGeometry ? float3(0, 0.5, 1) :
			reason == UnsupportedMaterial ? float3(1, 0.5, 0) : reason == MissingLight ? float3(1, 0, 1) :
			reason == MismatchedDepth ? float3(1, 1, 0) : float3(1, 1, 1);
		if (reason == MissingRoom) {
			uint roomStatus = uint(shadow.y);
			color = roomStatus == ReceiverRoom::MissingCoverage ? float3(0, 1, 1) :
				roomStatus == ReceiverRoom::UnwrittenMetadata ? float3(0, 0.25, 0.35) :
				roomStatus == ReceiverRoom::PartialCoverageWithCode ? float3(0.5, 1, 1) :
				roomStatus == ReceiverRoom::InvalidCode ? float3(0.5, 1, 0) :
				roomStatus == ReceiverRoom::MissingGeneration ? float3(1, 0.85, 0) :
				roomStatus == ReceiverRoom::StaleGeneration ? float3(0.2, 0.2, 1) :
				roomStatus == ReceiverRoom::MissingCode ? float3(0, 1, 0) : float3(0.1, 0.5, 0.5);
		}
		if (reason == ExcludedRoom) color = 0.25;
		if (reason == MissingLight) {
			uint status = uint(shadow.y);
			color = status == LightNoTable ? float3(0.5, 0, 0.5) : status == LightUnresolvedReference ? float3(0.5, 0.25, 0) :
				status == LightNoMatch ? float3(1, 0, 1) : status == LightAmbiguous ? float3(1, 0.5, 0.75) :
				status == LightUnsupported ? float3(0.5, 0, 1) : float3(1, 1, 1);
		}
		Preview[id.xy] = float4(color, 1);
	}
	else if (shadow.w == DirectRadianceTag) Preview[id.xy] = float4(pow(max(shadow.rgb, 0) / (1 + max(shadow.rgb, 0)), 1.0 / 2.2), 1);
	else if (shadow.w == 3) Preview[id.xy] = float4(shadow.rgb, 1);
	else if (shadow.w == 2) Preview[id.xy] = float4(1, 0, 1, 1);
	else if (shadow.y == 0) Preview[id.xy] = float4(1, 0, 0, 1);
	else Preview[id.xy] = float4(shadow.z.xxx, 1);
}

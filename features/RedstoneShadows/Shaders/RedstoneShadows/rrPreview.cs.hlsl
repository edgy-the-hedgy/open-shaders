#include "RedstoneShadows/directContract.hlsli"
Texture2D<float4> Source : register(t0);
Texture2D<float4> Paired : register(t1);
Texture2D<float4> NativeLighting : register(t2);
Texture2D<float4> NativeIndirect : register(t3);
RWTexture2D<float4> Preview : register(u0);
cbuffer PreviewSettings : register(b0)
{
	uint2 Size;
	uint2 NativeSize;
	uint Kind;
	uint Packed;
	uint Phase;
	uint Combined;
	float Exposure;
	uint Indirect;
	uint Unavailable;
	uint PreviewPadding;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= Size)) return;
	uint2 nativePixel = min(uint2((float2(id.xy) + 0.5) * NativeSize / Size), NativeSize - 1);
	float4 lighting = NativeLighting.Load(int3(nativePixel, 0));
	bool valid = lighting.w == DirectRadianceTag;
	float3 color = float3(1, 0, 1);
	if (Kind == 5) color = valid ? float3(0, 1, 0) : lighting.x == MismatchedDepth ? float3(1, 1, 0) : float3(1, 0, 1);
	else if (Unavailable != 0) color = float3(1, 0.3, 0);
	else if (!valid) color = lighting.x == MismatchedDepth ? float3(1, 1, 0) : float3(1, 0, 1);
	else if (Indirect != 0 && NativeIndirect.Load(int3(nativePixel, 0)).a < -1) color = float3(1, 0.3, 0);
	else if (Packed != 0 && ((id.x + id.y) & 1u) != Phase) color = float3(0.015, 0.015, 0.04);
	else {
		uint2 pixel = uint2(Packed != 0 ? id.x / 2 : id.x, id.y);
		float4 value = Source.Load(int3(pixel, 0));
		if (Combined != 0) value.rgb += Paired.Load(int3(pixel, 0)).rgb;
		if (!all(isfinite(value.rgb))) color = float3(1, 0, 0);
		else if (Kind == 1) color = (1.0 / (1.0 + max(value.x, 0) * exp2(-Exposure) * 0.001)).xxx;
		else if (Kind == 2) color = float3(saturate(value.xy * exp2(Exposure) * 10 + 0.5), 0.5);
		else if (Kind == 3) color = float3(value.xy, value.z);
		else if (Kind == 4) color = pow(saturate(value.rgb), 1.0 / 2.2);
		else { color = max(value.rgb * exp2(Exposure), 0); color = pow(color / (1 + color), 1.0 / 2.2); }
	}
	Preview[id.xy] = float4(color, 1);
}

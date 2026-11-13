cbuffer NativePBRIdentity : register(b13)
{
	uint DrawOrdinal;
	uint Reserved;
	uint MaterialLow;
	uint MaterialHigh;
};

struct Input
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

float4 main(Input input) : SV_Target0
{
	// One already-running depth-equal raster replay, no GPU readback or validation frame.
	// Low byte: preview mode. Bit 8: opaque PBR candidate without a published material identity.
	uint mode = Reserved & 0xffu;
	float2 gridUV = frac(input.uv * 8.0);
	float grid = 1.0 - step(0.035, min(gridUV.x, gridUV.y));
	if (mode == 2u) {
		// R/G show wrapped UVs; B shows the footprint of screen-space UV derivatives.
		float2 gradient = float2(length(ddx(input.uv)), length(ddy(input.uv)));
		float derivative = saturate(max(gradient.x, gradient.y) * 32.0);
		float3 uvColor = float3(frac(input.uv), derivative);
		return float4(lerp(uvColor, 1.0.xxx, grid * 0.4), 1.0);
	}
	if (mode == 3u) {
		// Green: captured and registered. Amber: same opaque PBR geometry scope, no valid registry identity.
		float3 coverageColor = (Reserved & 0x100u) != 0u ? float3(1.0, 0.28, 0.07) : float3(0.08, 0.85, 0.15);
		return float4(lerp(coverageColor, 1.0.xxx, grid * 0.5), 1.0);
	}
	// Original per-material identity preview.
	uint hash = MaterialLow ^ (MaterialHigh * 0x9e3779b9u);
	hash ^= hash >> 16;
	hash *= 0x7feb352du;
	hash ^= hash >> 15;
	float3 materialColor = 0.2 + 0.7 * float3((hash & 255u), ((hash >> 8) & 255u), ((hash >> 16) & 255u)) / 255.0;
	return float4(lerp(materialColor, 1.0.xxx, grid * 0.6), 1.0);
}

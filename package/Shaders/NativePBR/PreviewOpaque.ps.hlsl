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
	uint hash = MaterialLow ^ (MaterialHigh * 0x9e3779b9u);
	hash ^= hash >> 16;
	hash *= 0x7feb352du;
	hash ^= hash >> 15;
	float3 materialColor = 0.2 + 0.7 * float3((hash & 255u), ((hash >> 8) & 255u), ((hash >> 16) & 255u)) / 255.0;
	float2 gridUV = frac(input.uv * 8.0);
	float grid = 1.0 - step(0.035, min(gridUV.x, gridUV.y));
	return float4(lerp(materialColor, 1.0.xxx, grid * 0.6), 1.0);
}

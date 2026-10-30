#include "RedstoneShadows/directContract.hlsli"
Texture2D<float4> Lighting : register(t0);
Texture2D<float4> DirectDiffuse : register(t1);
Texture2D<float4> DirectSpecular : register(t2);
Texture2D<float4> NormalRoughness : register(t3);
Texture2D<float4> SpecularAlbedo : register(t4);
Texture2D<float4> DiffuseAlbedo : register(t5);
Texture2D<float4> IndirectDiffuse : register(t6);
Texture2D<float4> IndirectSpecular : register(t7);
RWTexture2D<float4> PackedDirectDiffuse : register(u0);
RWTexture2D<float4> PackedDirectSpecular : register(u1);
RWTexture2D<float4> PackedIndirectDiffuse : register(u2);
RWTexture2D<float4> PackedIndirectSpecular : register(u3);
RWTexture2D<float4> PackedNormal : register(u4);
RWTexture2D<float4> PackedSpecularAlbedo : register(u5);
RWTexture2D<float4> PackedDiffuseAlbedo : register(u6);
cbuffer RRGrid : register(b0) { uint2 NativeSize; uint2 RRSize; uint Checkerboard; uint Phase; uint2 GridPadding; };

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= RRSize)) return;
	uint2 pixel = min(uint2((float2(id.xy) + 0.5) * NativeSize / RRSize), NativeSize - 1);
	bool valid = Lighting.Load(int3(pixel, 0)).w == DirectRadianceTag;
	PackedNormal[id.xy] = valid ? NormalRoughness.Load(int3(pixel, 0)) : 0;
	PackedSpecularAlbedo[id.xy] = valid ? SpecularAlbedo.Load(int3(pixel, 0)) : 0;
	PackedDiffuseAlbedo[id.xy] = valid ? DiffuseAlbedo.Load(int3(pixel, 0)) : 0;
	if (Checkerboard != 0 && ((id.x + id.y) & 1u) != Phase) return;
	uint2 destination = uint2(Checkerboard != 0 ? id.x / 2 : id.x, id.y);
	PackedDirectDiffuse[destination] = valid ? DirectDiffuse.Load(int3(pixel, 0)) : 0;
	PackedDirectSpecular[destination] = valid ? DirectSpecular.Load(int3(pixel, 0)) : 0;
	float4 indirectDiffuse = IndirectDiffuse.Load(int3(pixel, 0)), indirectSpecular = IndirectSpecular.Load(int3(pixel, 0));
	PackedIndirectDiffuse[destination] = valid && indirectDiffuse.a >= -1 ? indirectDiffuse : float4(0, 0, 0, -1);
	PackedIndirectSpecular[destination] = valid && indirectSpecular.a >= -1 ? indirectSpecular : float4(0, 0, 0, -1);
}

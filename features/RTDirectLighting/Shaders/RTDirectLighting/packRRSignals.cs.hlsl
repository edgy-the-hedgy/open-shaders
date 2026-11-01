#include "RTDirectLighting/directContract.hlsli"
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
	uint2 pixel = id.xy; // Native-resolution guides; the signals alone may be half-width.
	const float4 direct = Lighting.Load(int3(pixel, 0));
	const float4 indirectDiffuse = IndirectDiffuse.Load(int3(pixel, 0));
	const float4 indirectSpecular = IndirectSpecular.Load(int3(pixel, 0));
	const bool directValid = direct.w == DirectRadianceTag && all(isfinite(direct.rgb)) && all(direct.rgb >= 0);
	// Spatial (-3) is a real provisional contribution for the compositor,
	// but it is not a native ray and must not become an RR input.
	const bool indirectValid = indirectDiffuse.a >= -1.0f && indirectSpecular.a >= -1.0f &&
		all(isfinite(indirectDiffuse)) && all(isfinite(indirectSpecular)) &&
		all(indirectDiffuse.rgb >= 0) && all(indirectSpecular.rgb >= 0);
	const float4 normalRoughness = NormalRoughness.Load(int3(pixel, 0));
	const bool guideValid = (directValid || indirectValid) && all(isfinite(normalRoughness)) &&
		any(normalRoughness.xy != 0.0f);
	PackedNormal[id.xy] = guideValid ? normalRoughness : 0;
	PackedSpecularAlbedo[id.xy] = guideValid ? SpecularAlbedo.Load(int3(pixel, 0)) : 0;
	PackedDiffuseAlbedo[id.xy] = guideValid ? DiffuseAlbedo.Load(int3(pixel, 0)) : 0;
	const bool directCB = (Checkerboard & 1u) != 0u;
	const bool indirectCB = (Checkerboard & 2u) != 0u;
	const uint directOrigin = Phase & 1u;
	const uint indirectOrigin = (Phase >> 1u) & 1u;
	// Pack only pixels actually selected by the corresponding trace mask.
	// A spatially reconstructed inactive pixel must never masquerade as a ray.
	const bool directSample = !directCB || ((pixel.x + pixel.y + directOrigin) & 1u) == 0u;
	const bool indirectSample = !indirectCB || ((pixel.x + pixel.y + indirectOrigin) & 1u) == 0u;
	const uint2 directDestination = uint2(directCB ? pixel.x / 2u : pixel.x, pixel.y);
	const uint2 indirectDestination = uint2(indirectCB ? pixel.x / 2u : pixel.x, pixel.y);
	if (directSample) {
		PackedDirectDiffuse[directDestination] = directValid && guideValid ? DirectDiffuse.Load(int3(pixel, 0)) : 0;
		PackedDirectSpecular[directDestination] = directValid && guideValid ? DirectSpecular.Load(int3(pixel, 0)) : 0;
	}
	if (indirectSample) {
		// -2 explicitly denotes a missing GI sample; never label it a hit.
		PackedIndirectDiffuse[indirectDestination] = indirectValid && guideValid ? indirectDiffuse : float4(0, 0, 0, -2);
		PackedIndirectSpecular[indirectDestination] = indirectValid && guideValid ? indirectSpecular : float4(0, 0, 0, -2);
	}
}

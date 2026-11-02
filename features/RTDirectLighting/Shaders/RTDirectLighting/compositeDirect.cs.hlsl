#ifndef CSHADER
#define CSHADER
#endif
#include "Common/Color.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/ReverseZ.hlsli"
#include "RTDirectLighting/directContract.hlsli"
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
Texture2D<float4> RawDirectDiffuseAlbedo : register(t15);
Texture2D<float4> ShadowMaskExport : register(t16);
RWTexture2D<float4> SceneColor : register(u0);
cbuffer LightingComposite : register(b0)
{
	uint UseIndirect;
	uint UseDirectRR;
	float IndirectStrength;
	uint UseDXRIndirect;
	float RedstoneBlend;
	uint ShadowOnlyEnabled;
	float ShadowInfluence;
	float ShadowAuraRadius;
};

// An intentionally non-surface-aware dilation: source samples must be
// confirmed shadowed, but the destination pixel may have invalid geometry,
// no receiver, or no shadow coverage of its own.
float4 ApplyShadowAura(uint2 pixel, uint2 extent, float4 color)
{
    if (ShadowOnlyEnabled == 0u) return color;
    const int radius = clamp(int(ShadowAuraRadius + 0.5), 0, 3);
    float visibility = 1.0f;
    [loop] for (int dy = -radius; dy <= radius; ++dy) {
        [loop] for (int dx = -radius; dx <= radius; ++dx) {
            const int2 neighbor = int2(pixel) + int2(dx, dy);
            if (any(neighbor < 0) || any(neighbor >= int2(extent))) continue;
            const float2 mask = ShadowMaskExport.Load(int3(neighbor, 0)).rg;
            if (all(isfinite(mask)) && mask.y >= 0.5f)
                visibility = min(visibility, saturate(mask.x));
        }
    }
    if (visibility >= 1.0f) return color;
    const float attenuation = lerp(1.0f, visibility, saturate(ShadowInfluence));
    const float3 linearColor = Color::SceneGammaToLinear(color.rgb);
    return float4(Color::SceneLinearToGamma(linearColor * attenuation), color.a);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint width, height;
	DirectLighting.GetDimensions(width, height);
	if (id.x >= width || id.y >= height) return;
	uint targetWidth, targetHeight;
	SceneColor.GetDimensions(targetWidth, targetHeight);
	if (id.x >= targetWidth || id.y >= targetHeight) return;
    // Shadow aura executes LAST, even for invalid direct receivers or a
    // disabled direct blend. It may overwrite originally invalid mask targets.
    if (RedstoneBlend <= 0.0f) {
        SceneColor[id.xy] = ApplyShadowAura(id.xy, uint2(width, height), SceneColor[id.xy]);
        return;
    }
	float4 lighting = DirectLighting.Load(int3(id.xy, 0));
    if (lighting.w != DirectRadianceTag || !all(isfinite(lighting.rgb))) {
        SceneColor[id.xy] = ApplyShadowAura(id.xy, uint2(width, height), SceneColor[id.xy]);
        return;
    }
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
			{
                const float3 emission = float3(RawDirectDiffuse.Load(int3(id.xy, 0)).a,
                    RawDirectSpecular.Load(int3(id.xy, 0)).a, RawDirectSpecularAlbedo.Load(int3(id.xy, 0)).a);
                const float3 diffuseAlbedo = RawDirectDiffuseAlbedo.Load(int3(id.xy, 0)).rgb;
                const float3 specularAlbedo = RawDirectSpecularAlbedo.Load(int3(id.xy, 0)).rgb;
                lighting.rgb = emission + filteredDiffuse * diffuseAlbedo + filteredSpecular * specularAlbedo;
            }
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
    if (!all(isfinite(working))) {
        SceneColor[id.xy] = ApplyShadowAura(id.xy, uint2(width, height), SceneColor[id.xy]);
        return;
    }
	if (RedstoneBlend >= 1.0f) {
        const float4 composed = float4(Color::SceneLinearToGamma(working), 1.0f);
        SceneColor[id.xy] = ApplyShadowAura(id.xy, uint2(width, height), composed);
        return;
    }

    float4 raster = SceneColor[id.xy];
    float3 rasterLinear = Color::SceneGammaToLinear(raster.rgb);
    float3 blendedLinear = lerp(rasterLinear, working, saturate(RedstoneBlend));

    SceneColor[id.xy] = ApplyShadowAura(id.xy, uint2(width, height),
        float4(Color::SceneLinearToGamma(blendedLinear), raster.a));
}

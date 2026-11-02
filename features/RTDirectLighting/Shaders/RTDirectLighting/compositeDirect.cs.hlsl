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
	float ShadowEdgeFilterEnabled;
};

// Valid-receiver-only, center-weighted shadow edge AA; never expand shadows
// onto invalid geometry. Reject taps across depth, normal and room boundaries.
float ContactShadowVisibility(uint2 pixel, uint2 extent)
{
    const float2 mask = ShadowMaskExport.Load(int3(pixel, 0)).rg;
    if (!all(isfinite(mask)) || mask.y < 0.5f) return 1.0f;
    const float center = saturate(mask.x);
    if (ShadowEdgeFilterEnabled < 0.5f) return center; // Custom unchanged
    const float4 receiver = NativeReceivers.Load(int3(pixel, 0));
    if (!all(isfinite(receiver)) || receiver.w == 0.0f) return 1.0f;
    const float depth = abs(mul(FrameBuffer::CameraView[0], float4(receiver.xyz, 1.0f)).z);
    if (!isfinite(depth) || depth <= 0.0f) return 1.0f;
    const float2 normalXY = NativeNormalRoughness.Load(int3(pixel, 0)).xy;
    if (!all(isfinite(normalXY))) return center;
    const float3 normal = GBuffer::DecodeNormal(normalXY);
    float total = 4.0f * center, weights = 4.0f;
    [unroll] for (int dy = -1; dy <= 1; ++dy) {
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            const int2 tap = int2(pixel) + int2(dx, dy);
            if (any(tap < 0) || any(tap >= int2(extent))) continue;
            const float2 neighborMask = ShadowMaskExport.Load(int3(tap, 0)).rg;
            const float4 neighbor = NativeReceivers.Load(int3(tap, 0));
            if (!all(isfinite(neighborMask)) || neighborMask.y < 0.5f ||
                !all(isfinite(neighbor)) || neighbor.w != receiver.w) continue;
            const float neighborDepth = abs(mul(FrameBuffer::CameraView[0],
                float4(neighbor.xyz, 1.0f)).z);
            if (!isfinite(neighborDepth) ||
                abs(neighborDepth - depth) > max(0.06f, depth * 0.0125f)) continue;
            const float2 neighborNormal = NativeNormalRoughness.Load(int3(tap, 0)).xy;
            if (!all(isfinite(neighborNormal)) ||
                dot(normal, GBuffer::DecodeNormal(neighborNormal)) < 0.82f) continue;
            const float weight = (dx == 0 || dy == 0) ? 2.0f : 1.0f;
            total += weight * saturate(neighborMask.x);
            weights += weight;
        }
    }
    return lerp(center, total / weights, 0.65f);
}

float4 ApplyContactShadow(uint2 pixel, uint2 extent, float4 color)
{
    if (ShadowOnlyEnabled == 0u) return color;
    const float visibility = ContactShadowVisibility(pixel, extent);
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
    // Apply at the final color once, but only for this pixel's valid receiver.
    if (RedstoneBlend <= 0.0f) {
        SceneColor[id.xy] = ApplyContactShadow(id.xy, uint2(width, height), SceneColor[id.xy]);
        return;
    }
	float4 lighting = DirectLighting.Load(int3(id.xy, 0));
    if (lighting.w != DirectRadianceTag || !all(isfinite(lighting.rgb))) {
        SceneColor[id.xy] = ApplyContactShadow(id.xy, uint2(width, height), SceneColor[id.xy]);
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
        // Stochastic per-ray lobe selection may leave just one family sampled.
        // Preserve that family's radiance instead of requiring both -2 validity
        // tags to be present. Unsampled families contribute zero, never stale RR.
        const bool validD = diffuse.a >= -1.0 && all(isfinite(diffuse.rgb));
        const bool validS = specular.a >= -1.0 && all(isfinite(specular.rgb));
        if (validD || validS) {
            float3 d = validD ? diffuse.rgb : 0.0;
            float3 s = validS ? specular.rgb : 0.0;
            if ((rrFlags & 2u) != 0) {
                const float3 denoisedD = FilteredIndirectDiffuse.Load(int3(rrPixel, 0)).rgb;
                const float3 denoisedS = FilteredIndirectSpecular.Load(int3(rrPixel, 0)).rgb;
                if (validD && all(isfinite(denoisedD)) && all(denoisedD >= 0.0)) d = denoisedD;
                if (validS && all(isfinite(denoisedS)) && all(denoisedS >= 0.0)) s = denoisedS;
            }
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
        SceneColor[id.xy] = ApplyContactShadow(id.xy, uint2(width, height), SceneColor[id.xy]);
        return;
    }
	if (RedstoneBlend >= 1.0f) {
        const float4 composed = float4(Color::SceneLinearToGamma(working), 1.0f);
        SceneColor[id.xy] = ApplyContactShadow(id.xy, uint2(width, height), composed);
        return;
    }

    float4 raster = SceneColor[id.xy];
    float3 rasterLinear = Color::SceneGammaToLinear(raster.rgb);
    float3 blendedLinear = lerp(rasterLinear, working, saturate(RedstoneBlend));

    SceneColor[id.xy] = ApplyContactShadow(id.xy, uint2(width, height),
        float4(Color::SceneLinearToGamma(blendedLinear), raster.a));
}

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
RWTexture2D<float4> SceneColor : register(u0);
cbuffer LightingComposite : register(b0)
{
	uint UseIndirect;
	uint UseDirectRR;
	float IndirectStrength;
	uint UseDXRIndirect;
	float RedstoneBlend;
	uint UseRasterFallback;
	float2 Padding;
};

// Direct lighting is a replaceable baseline; GI is an additive independent
// contribution. Never substitute the GI-only sum for an incomplete raster pixel.
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    DirectLighting.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    uint targetWidth, targetHeight;
    SceneColor.GetDimensions(targetWidth, targetHeight);
    if (id.x >= targetWidth || id.y >= targetHeight) return;
    // Strict RT-only never inherits raster, even when the legacy blend
    // slider is zero. Raster rendering is still allowed for HUD/UI passes.
    const bool strictRTOnly = UseRasterFallback == 0u;
    if (!strictRTOnly && RedstoneBlend <= 0.0f) return;

    const float4 rawDirect = DirectLighting.Load(int3(id.xy, 0));
    const bool directRaw = rawDirect.w == DirectRadianceTag;
    const bool directValid = (directRaw || rawDirect.w == ReconstructedDirectRadianceTag) &&
        all(isfinite(rawDirect.rgb)) && all(rawDirect.rgb >= 0.0f);
    const float4 rawIndirectD = UseDXRIndirect != 0 ?
        RawIndirectDiffuse.Load(int3(id.xy, 0)) : float4(0, 0, 0, -2);
    const float4 rawIndirectS = UseDXRIndirect != 0 ?
        RawIndirectSpecular.Load(int3(id.xy, 0)) : float4(0, 0, 0, -2);
    const bool giRaw = rawIndirectD.a >= -1.0f && rawIndirectS.a >= -1.0f;
    const bool giSpatial = rawIndirectD.a == ReconstructedIndirectDistanceTag &&
        rawIndirectS.a == ReconstructedIndirectDistanceTag;
    const bool giValid = UseDXRIndirect != 0 && (giRaw || giSpatial) &&
        all(isfinite(rawIndirectD)) && all(isfinite(rawIndirectS)) &&
        all(rawIndirectD.rgb >= 0.0f) && all(rawIndirectS.rgb >= 0.0f);
    // With spatial reconstruction disabled, unsampled RT pixels must
    // never silently inherit raster unless fallback was explicitly enabled.
    // Still allow GI to contribute even when direct lighting is unavailable.
    if (!directValid && !giValid && (strictRTOnly || UseIndirect == 0u)) {
        if (strictRTOnly)
            SceneColor[id.xy] = float4(0.0f, 0.0f, 0.0f, SceneColor[id.xy].a);
        return;
    }

    uint rrFlags = UseDirectRR;
    uint2 rrPixel = 0;
    if (rrFlags != 0u) {
        uint rrWidth, rrHeight;
        RRDepth.GetDimensions(rrWidth, rrHeight);
        if (rrWidth == 0u || rrHeight == 0u) {
            rrFlags = 0u;
        } else {
            rrPixel = min(id.xy, uint2(rrWidth, rrHeight) - 1u);
            const float4 receiver = NativeReceivers.Load(int3(id.xy, 0));
            const float depth = mul(FrameBuffer::CameraView[0], float4(receiver.xyz, 1)).z;
            const float4 guide = RRNormalRoughness.Load(int3(rrPixel, 0));
            const float4 sourceGuide = RawNormalRoughness.Load(int3(id.xy, 0));
            if (receiver.w == 0 || !all(isfinite(receiver)) ||
                !isfinite(depth) || !isfinite(RRDepth.Load(int3(rrPixel, 0))) ||
                abs(RRDepth.Load(int3(rrPixel, 0)) - depth) > max(0.5f, abs(depth) * 0.02f) ||
                !all(isfinite(guide)) || !all(isfinite(sourceGuide)) ||
                dot(GBuffer::DecodeNormal(guide.xy), GBuffer::DecodeNormal(sourceGuide.xy)) < 0.9f ||
                abs(guide.z - sourceGuide.z) > 0.1f)
                rrFlags = 0u;
        }
    }

    float3 directRGB = directValid ? rawDirect.rgb : 0.0f;
    if (directRaw && (rrFlags & 1u) != 0u) {
        const float3 filteredDiffuse = FilteredDirectDiffuse.Load(int3(rrPixel, 0)).rgb;
        const float3 filteredSpecular = FilteredDirectSpecular.Load(int3(rrPixel, 0)).rgb;
        if (all(isfinite(filteredDiffuse)) && all(isfinite(filteredSpecular)) &&
            all(filteredDiffuse >= 0.0f) && all(filteredSpecular >= 0.0f)) {
            const float3 emission = float3(
                RawDirectDiffuse.Load(int3(id.xy, 0)).a,
                RawDirectSpecular.Load(int3(id.xy, 0)).a,
                RawDirectSpecularAlbedo.Load(int3(id.xy, 0)).a);
            const float3 diffuseAlbedo = RawDirectDiffuseAlbedo.Load(int3(id.xy, 0)).rgb;
            const float3 specularAlbedo = RawDirectSpecularAlbedo.Load(int3(id.xy, 0)).rgb;
            if (all(isfinite(emission)) && all(isfinite(diffuseAlbedo)) &&
                all(isfinite(specularAlbedo)))
                directRGB = emission + filteredDiffuse * diffuseAlbedo + filteredSpecular * specularAlbedo;
        }
    }

    // RR and raw GI validity are independent of the direct radiance tag.
    float3 indirectRGB = 0.0f;
    if (giValid) {
        float3 diffuse = rawIndirectD.rgb;
        float3 specular = rawIndirectS.rgb;
        if (giRaw && (rrFlags & 2u) != 0u) {
            const float3 filteredD = FilteredIndirectDiffuse.Load(int3(rrPixel, 0)).rgb;
            const float3 filteredS = FilteredIndirectSpecular.Load(int3(rrPixel, 0)).rgb;
            if (all(isfinite(filteredD)) && all(isfinite(filteredS)) &&
                all(filteredD >= 0.0f) && all(filteredS >= 0.0f)) {
                diffuse = filteredD;
                specular = filteredS;
            }
        }
        indirectRGB = (diffuse + specular) * IndirectStrength;
    }

    const float4 raster = SceneColor[id.xy];
    const float3 rasterLinear = Color::SceneGammaToLinear(raster.rgb);
    // Missing native direct light chooses raster or zero baseline, WITHOUT
    // altering GI validity. GI-only samples remain visible over a black base.
    const bool rasterFallback = UseRasterFallback != 0u;
    const float3 missingDirectBase = rasterFallback ? rasterLinear : float3(0.0f, 0.0f, 0.0f);
    float3 working = directValid ?
        (ENABLE_LL ? Color::GamutTransform(directRGB) : directRGB) : missingDirectBase;
    working += indirectRGB;

    // Hybrid screen-space GI is not a native traced signal. Exclude it
    // from strict isolation so only native direct and RT-exclusive GI show.
    if (!strictRTOnly && UseIndirect != 0u) {
        const float4 nr = NativeNormalRoughness.Load(int3(id.xy, 0));
        const float3 normal = normalize(mul((float3x3)FrameBuffer::CameraViewInverse[0],
            GBuffer::DecodeNormal(nr.xy)));
        float3 diffuse, specular;
        float2 visibility;
        SampleRedstone(id.xy, normal, 1.0f - nr.z, diffuse, specular, visibility);
        working += (diffuse + specular) * IndirectStrength;
    }
    if (!all(isfinite(working))) {
        if (!rasterFallback)
            SceneColor[id.xy] = float4(0.0f, 0.0f, 0.0f, raster.a);
        return;
    }

    // The strict path is a true native-only image (no SceneColor RGB and
    // no raster blending). With fallback enabled, preserve normal blending.
    const float3 result = strictRTOnly ? working :
        lerp(rasterLinear, working, saturate(RedstoneBlend));
    SceneColor[id.xy] = float4(Color::SceneLinearToGamma(result), raster.a);
}

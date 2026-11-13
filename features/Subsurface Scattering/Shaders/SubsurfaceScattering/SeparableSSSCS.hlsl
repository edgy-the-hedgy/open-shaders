RWTexture2D<float4> SSSRW : register(u0);

Texture2D<float4> ColorTexture : register(t0);
Texture2D<float4> DepthTexture : register(t1);
Texture2D<float4> MaskTexture : register(t2);
Texture2D<float4> AlbedoTexture : register(t3);
Texture2D<float4> NormalTexture : register(t4);

SamplerState PointSampler : register(s0);

#include "Common/Color.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "SubsurfaceScattering/SSSCommon.hlsli"

#if defined(BURLEY)
#	include "SubsurfaceScattering/Burley.hlsli"
#else
#	include "SubsurfaceScattering/SeparableSSS.hlsli"
#endif

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	// Early exit if dispatch thread is outside screen bounds
	if (any(DTid.xy >= uint2(SharedData::BufferDim.xy)))
		return;

	float2 texCoord = (DTid.xy + 0.5) * SharedData::BufferDim.zw;
	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(texCoord);

#if defined(BURLEY)

	float sssAmount = MaskTexture[DTid.xy].x;

	if (sssAmount > 0.0) {
		bool humanProfile = MaskTexture[DTid.xy].y > 0.0;

		float4 color = BurleyNormalizedSS(DTid.xy, texCoord, eyeIndex, sssAmount, humanProfile, SSSRW[DTid.xy]);
		SSSRW[DTid.xy] = max(0, color);
	}

#elif defined(HORIZONTAL)

	float sssAmount = MaskTexture[DTid.xy].x;
	bool humanProfile = MaskTexture[DTid.xy].y > 0.0;

	float4 color = SSSSBlurCS(texCoord, float2(1.0, 0.0), sssAmount, humanProfile);
	SSSRW[DTid.xy] = max(0, color);

#else

	float sssAmount = MaskTexture[DTid.xy].x;

	if (sssAmount > 0.0) {
		bool humanProfile = MaskTexture[DTid.xy].y > 0.0;

		float4 originalColor = SSSRW[DTid.xy];
		float4 color = SSSSBlurCS(texCoord, float2(0.0, 1.0), sssAmount, humanProfile);
		float3 albedo = SSSDecodeAlbedo(AlbedoTexture[DTid.xy].rgb);
<<<<<<< HEAD
		color.rgb = SSSApplyAlbedo(color.rgb, Color::SceneGammaToLinear(originalColor.rgb), albedo, ScatterMode);
		color.rgb = Color::SceneLinearToGamma(color.rgb);
||||||| 12b261724
		color.rgb = SSSApplyAlbedo(color.rgb, Color::IrradianceToLinear(originalColor.rgb), albedo, ScatterMode);
		color.rgb = Color::IrradianceToGamma(color.rgb);
=======
		color.rgb = SSSApplyAlbedo(color.rgb, ColorManagement::SceneToLinear(originalColor.rgb), albedo, ScatterMode);
		color.rgb = ColorManagement::LinearToScene(color.rgb);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
		SSSRW[DTid.xy] = float4(color.rgb, originalColor.a);
	}

#endif
}

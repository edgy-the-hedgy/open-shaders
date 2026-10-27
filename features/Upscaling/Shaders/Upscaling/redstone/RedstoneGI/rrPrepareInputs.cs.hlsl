#include "Common/Color.hlsli"
#include "Common/GBuffer.hlsli"
#include "Upscaling/redstone/RedstoneGI/common.hlsli"

Texture2D<float> srcWorkingDepth : register(t0);
Texture2D<float4> srcNormalRoughness : register(t1);
Texture2D<float4> srcMotionVectors : register(t2);
Texture2D<float4> srcSpecularAlbedo : register(t3);  // Open Shaders REFLECTANCE: split-sum specular lobe weight
Texture2D<float4> srcDiffuseAlbedo : register(t4);
Texture2D<float> srcPreviousLinearDepth : register(t5);

RWTexture2D<float> outLinearDepth : register(u0);
RWTexture2D<float4> outMotionVectors : register(u1);
RWTexture2D<float4> outNormalRoughness : register(u2);
RWTexture2D<float4> outSpecularAlbedo : register(u3);
RWTexture2D<float4> outDiffuseAlbedo : register(u4);

/** @brief Builds current-frame RR depth, three-component motion, normals, and material feature maps. */
[numthreads(8, 8, 1)] void main(uint2 dtid : SV_DispatchThreadID) {
	if (any(dtid >= uint2(OUT_FRAME_DIM)))
		return;

	// SSGI working depth is already signed view-space linear depth.
	uint2 surfacePixel = SelectSurfacePixel(srcWorkingDepth, dtid);
	const float2 surfaceUV = (surfacePixel + 0.5) * RcpFrameDim;
	const float depth = srcWorkingDepth.Load(int3(surfacePixel, 0));
	outLinearDepth[dtid] = depth;

	float2 motion = srcMotionVectors.Load(int3(surfacePixel, 0)).xy;
	motion = all(isfinite(motion)) ? motion : 0;
	float depthDelta = 0;
	const float2 previousUV = surfaceUV + motion;
	if (RRHistoryValid != 0 && all(previousUV >= 0) && all(previousUV < 1) && isfinite(depth) && depth > 0) {
		const uint2 historySize = uint2(OUT_FRAME_DIM);
		const uint2 previousPixel = min(uint2(previousUV * historySize), historySize - 1u);
		const float previousDepth = srcPreviousLinearDepth.Load(int3(previousPixel, 0));
		const float delta = previousDepth - depth;
		if (isfinite(previousDepth) && previousDepth > 0 && isfinite(delta)) depthDelta = delta;
	}
	outMotionVectors[dtid] = float4(motion, depthDelta, 0);

	const float4 nr = srcNormalRoughness.Load(int3(surfacePixel, 0));

	// Passing GBuffer encoding directly would give RR inverted view-space normals instead of world-space normals.
	float3 rrNormal = normalize(ViewToWorldVector(GBuffer::DecodeNormal(nr.xy), FrameBuffer::CameraViewInverse[Stereo::GetEyeIndexFromTexCoord(surfaceUV)]));
	rrNormal /= max(abs(rrNormal.x) + abs(rrNormal.y) + abs(rrNormal.z), 1e-6);
	float2 rrNormalOct = rrNormal.xy;
	if (rrNormal.z < 0.0)
		rrNormalOct = (1.0 - abs(rrNormalOct.yx)) * (rrNormalOct.xy >= 0.0 ? 1.0 : -1.0);
	rrNormalOct = rrNormalOct * 0.5 + 0.5;

	// The guide must describe the same roughness floor used to generate SSGI specular radiance.
	outNormalRoughness[dtid] = float4(rrNormalOct, max(SpecularRoughnessFloor, saturate(1.0 - nr.z)), 0.0);

	// These feature maps are linear, matching FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO.
	outSpecularAlbedo[dtid] = float4(saturate(srcSpecularAlbedo.Load(int3(surfacePixel, 0)).rgb), 1.0);
	outDiffuseAlbedo[dtid] = float4(saturate(Color::IrradianceToLinear(srcDiffuseAlbedo.Load(int3(surfacePixel, 0)).rgb / Color::PBRLightingScale)), 1.0);
}

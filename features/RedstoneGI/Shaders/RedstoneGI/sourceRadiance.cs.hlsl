#include "Common/Color.hlsli"
#include "RedstoneGI/common.hlsli"
Texture2D<float3> Source : register(t0);
Texture2D<float> srcWorkingDepth : register(t1);
RWTexture2D<float3> Radiance : register(u0);
[numthreads(8,8,1)] void main(uint2 pixel : SV_DispatchThreadID)
{
	if (any(pixel >= uint2(OUT_FRAME_DIM))) return;
	uint2 surfacePixel = SelectSurfacePixel(srcWorkingDepth, pixel);
	float3 source = Source.Load(int3(surfacePixel, 0));
	float3 value = ENABLE_LL ? Color::SceneGammaToLinear(source) * GIStrength : Color::RadianceToLinear(source * GIStrength);
	Radiance[pixel] = all(isfinite(value)) ? max(value, 0) : 0;
}

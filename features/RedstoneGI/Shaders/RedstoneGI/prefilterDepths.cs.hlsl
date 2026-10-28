#include "RedstoneGI/common.hlsli"
Texture2D<float> srcNDCDepth : register(t0);
RWTexture2D<float> outDepth : register(u0);

[numthreads(8,8,1)] void main(uint2 pixel : SV_DispatchThreadID)
{
	if (any(pixel >= uint2(FrameDim))) return;
	float raw = srcNDCDepth.Load(int3(pixel, 0));
	float depth = ScreenToViewDepth(raw);
	outDepth[pixel] = raw != FrameBuffer::FarPlaneDepth() && isfinite(depth) && depth > 0 ? depth : 0;
}

#include "Common/ReverseZ.hlsli"

Texture2D<float> Depth : register(t0);
RWTexture2D<float4> Receivers : register(u0);
cbuffer Grid : register(b0)
{
	uint2 Size;
	uint2 NativeSize;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= Size)) return;
	uint2 pixel = min(uint2((float2(id.xy) + 0.5) * NativeSize / Size), NativeSize - 1);
	float raw = Depth.Load(int3(pixel, 0));
	float depth = FrameBuffer::ToStandardDepth(raw);
	float2 uv = (float2(pixel) + 0.5) / NativeSize;
	float projectedDepth = FrameBuffer::IsReverseProjection() ? 1.0 - depth : depth;
	float4 view = mul(FrameBuffer::CameraProjInverse[0], float4(uv * float2(2, -2) + float2(-1, 1), projectedDepth, 1));
	bool valid = isfinite(raw) && raw != FrameBuffer::FarPlaneDepth() && depth >= 0 && depth < 1 && abs(view.w) > 1e-8;
	float3 relative = mul(FrameBuffer::CameraViewInverse[0], float4(view.xyz / view.w, 1)).xyz;
	valid = valid && all(isfinite(relative));
	Receivers[id.xy] = valid ? float4(relative, 1) : 0;
}

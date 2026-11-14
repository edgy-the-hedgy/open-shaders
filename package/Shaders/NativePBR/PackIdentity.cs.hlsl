Texture2D<uint2> IdentityAndColor : register(t0);
RWTexture2D<uint4> OrdinalBytes : register(u0);
RWTexture2D<uint4> VertexColorBytes : register(u1);

uint4 PackBytes(uint value)
{
	return uint4(value & 255u, (value >> 8u) & 255u, (value >> 16u) & 255u, value >> 24u);
}

[numthreads(8, 8, 1)]
void main(uint3 threadId : SV_DispatchThreadID)
{
	uint width, height;
	IdentityAndColor.GetDimensions(width, height);
	if (threadId.x >= width || threadId.y >= height)
		return;
	uint2 values = IdentityAndColor[threadId.xy];
	OrdinalBytes[threadId.xy] = PackBytes(values.x);
	VertexColorBytes[threadId.xy] = PackBytes(values.y);
}

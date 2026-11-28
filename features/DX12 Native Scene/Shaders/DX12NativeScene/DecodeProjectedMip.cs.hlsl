// Transfer decoded texels from the exact engine SRV, including its mip range.
// sRGB SRVs return linear values; output is a linear UNORM or FLOAT texture.
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);
cbuffer DecodeConstants : register(b0)
{
	uint Width;
	uint Height;
	uint OffsetY;
	uint Mip;
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= Width || id.y >= Height)
		return;
	Destination[uint2(id.x, id.y + OffsetY)] = Source.Load(int3(id.xy, Mip));
}

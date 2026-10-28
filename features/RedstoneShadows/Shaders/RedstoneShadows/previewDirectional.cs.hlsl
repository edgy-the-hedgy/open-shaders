Texture2D<float4> Shadow : register(t0);
RWTexture2D<float4> Preview : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint width, height;
	Preview.GetDimensions(width, height);
	if (id.x >= width || id.y >= height) return;
	float4 shadow = Shadow.Load(int3(id.xy, 0));
	if (shadow.w == 0) Preview[id.xy] = float4(0, 0, 0, 1);
	else if (shadow.y == 0) Preview[id.xy] = float4(1, 0, 0, 1);
	else Preview[id.xy] = float4(shadow.z.xxx, 1);
}

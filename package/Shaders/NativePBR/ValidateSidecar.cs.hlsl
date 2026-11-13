Texture2D<float> FinalOpaqueDepth : register(t0);
Texture2D<float4> SidecarDdyDepthFacing : register(t1);
RWTexture2D<uint> SidecarIdentity : register(u0);

cbuffer Validation : register(b0)
{
    uint Width;
    uint Height;
    uint DepthIs24Bit;
    uint Reserved;
};

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 xy = dispatchId.xy;
    if (xy.x >= Width || xy.y >= Height)
        return;
    uint identity = SidecarIdentity[xy];
    if (identity == 0)
        return;
    float written = SidecarDdyDepthFacing.Load(int3(xy, 0)).z;
    float finalDepth = FinalOpaqueDepth.Load(int3(xy, 0));
    // A later legacy mesh can occlude an earlier PBR draw. Clear its coverage.
    // D24 uses the same quantization-aware comparison as the CPU contract.
    float tolerance = DepthIs24Bit != 0 ? 0.5f / 16777215.0f : 0.0f;
    if (!isfinite(written) || !isfinite(finalDepth) || written < 0 || written > 1 ||
        finalDepth < 0 || finalDepth > 1 || abs(written - finalDepth) > tolerance)
        SidecarIdentity[xy] = 0;
}

// FXC SM5 capture only: no material evaluation, lighting, or legacy MRT writes.
// The actual material evaluator belongs to DX12/SM6.5 (a later commit).
cbuffer NativePBRIdentity : register(b13)
{
    uint DrawOrdinal;
    uint Reserved;
    uint MaterialLow;
    uint MaterialHigh;
};

struct Input
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 basis0 : TEXCOORD1;
    float3 basis1 : TEXCOORD2;
    float3 basis2 : TEXCOORD3;
};

struct Output
{
    uint identity : SV_Target0;
    float4 uvDdx : SV_Target1;
    float4 ddyDepthFacing : SV_Target2;
    float4 basis0 : SV_Target3;
    float4 basis1 : SV_Target4;
    float4 basis2 : SV_Target5;
};

Output main(Input input, bool frontFacing : SV_IsFrontFace)
{
    Output output;
    output.identity = DrawOrdinal;
    output.uvDdx = float4(input.uv, ddx(input.uv));
    output.ddyDepthFacing = float4(ddy(input.uv), input.position.z, frontFacing ? 1.0f : -1.0f);
    output.basis0 = float4(input.basis0, 0);
    output.basis1 = float4(input.basis1, 0);
    output.basis2 = float4(input.basis2, 0);
    return output;
}

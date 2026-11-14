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
#ifdef CAPTURE_PROJECTED_UV
    float4 uv : TEXCOORD0;
#else
    float2 uv : TEXCOORD0;
#endif
    float3 basis0 : TEXCOORD1;
    float3 basis1 : TEXCOORD2;
    float3 basis2 : TEXCOORD3;
    float4 vertexColor : COLOR0;
#ifdef CAPTURE_PROJECTED_UV
    float4 worldPosition : POSITION1;
    float3 projectionDirection : TEXCOORD7;
#endif
};

struct Output
{
    uint2 identityAndColor : SV_Target0;
    float4 uvDdx : SV_Target1;
    float4 ddyDepthFacing : SV_Target2;
    float4 basis0 : SV_Target3;
    float4 basis1 : SV_Target4;
    float4 basis2 : SV_Target5;
#ifdef CAPTURE_PROJECTED_UV
    float4 worldPosition : SV_Target6;
    float4 projectionDirection : SV_Target7;
#endif
};

Output main(Input input, bool frontFacing : SV_IsFrontFace)
{
    Output output;
    uint4 rgba8 = (uint4)round(saturate(input.vertexColor) * 255.0f);
    output.identityAndColor = uint2(DrawOrdinal,
        rgba8.r | (rgba8.g << 8) | (rgba8.b << 16) | (rgba8.a << 24));
    output.uvDdx = float4(input.uv.xy, ddx(input.uv.xy));
    output.ddyDepthFacing = float4(ddy(input.uv.xy), input.position.z, frontFacing ? 1.0f : -1.0f);
#ifdef CAPTURE_PROJECTED_UV
    output.basis0 = float4(input.basis0, input.vertexColor.a);
#else
    output.basis0 = float4(input.basis0, 0);
#endif
    output.basis1 = float4(input.basis1, 0);
    output.basis2 = float4(input.basis2, 0);
#ifdef CAPTURE_PROJECTED_UV
    output.worldPosition = float4(input.worldPosition.xyz, input.uv.z);
    output.projectionDirection = float4(input.projectionDirection, input.uv.w);
#endif
    return output;
}

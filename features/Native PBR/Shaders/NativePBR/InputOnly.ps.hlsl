// Full Native original opaque raster draw: raw inputs plus engine compatibility.
// No material textures, BRDF, lighting, shadow or environment evaluation.
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/MotionBlur.hlsli"
struct RasterInput
{
    float4 position : SV_POSITION0;
    float2 uv : TEXCOORD0;
    float3 tbn0 : TEXCOORD1;
    float3 tbn1 : TEXCOORD2;
    float3 tbn2 : TEXCOORD3;
    float4 worldPosition : POSITION1;
    float4 previousWorldPosition : POSITION2;
    float4 color : COLOR0;
    float4 fogParam : COLOR1;
    float3 modelPosition : TEXCOORD12;
};
cbuffer DrawIdentity : register(b0)
{
    uint Instance;
    uint TokenBase;
    uint PrimitiveCount;
    uint Reserved;
};
struct RawOutput
{
    float4 diffuse : SV_Target0;
    float4 motion : SV_Target1;
    float4 normal : SV_Target2;
    uint4 identityUV : SV_Target3;
    uint4 colorGradDepth : SV_Target4;
    uint4 basisFlags : SV_Target5;
    float4 masks : SV_Target6;
    float4 evidence : SV_Target7;
};
uint PackHalf2(float2 value)
{
    uint2 bits = f32tof16(clamp(value, -65504.0, 65504.0));
    return bits.x | (bits.y << 16);
}
uint PackOct(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-8);
    float2 p = n.xy;
    if (n.z < 0)
        p = (1.0 - abs(p.yx)) * float2(p.x >= 0 ? 1.0 : -1.0, p.y >= 0 ? 1.0 : -1.0);
    uint2 q = uint2(round(saturate(p * 0.5 + 0.5) * 65535.0));
    return q.x | (q.y << 16);
}
[earlydepthstencil]
RawOutput main(RasterInput input, uint primitive : SV_PrimitiveID, bool front : SV_IsFrontFace)
{
    RawOutput result = (RawOutput)0;
    float2 dx = ddx(input.uv);
    float2 dy = ddy(input.uv);
    float3x3 basis = transpose(float3x3(input.tbn0, input.tbn1, input.tbn2));
    result.diffuse.a = 1.0;
    result.motion = float4(MotionBlur::GetSSMotionVector(input.worldPosition, input.previousWorldPosition), 0.0, 1.0);
    result.evidence.a = 1.0;
    result.masks.a = 1.0;
    if (Instance == 0xffffffffu || TokenBase == 0 || primitive >= PrimitiveCount ||
        TokenBase + primitive > 65535u || !all(isfinite(input.uv)) ||
        !all(isfinite(dx)) || !all(isfinite(dy)) || !all(isfinite(input.color)) ||
        !all(isfinite(input.position)) ||
        !all(isfinite(basis[0])) || !all(isfinite(basis[1])) || !all(isfinite(basis[2])) ||
        dot(basis[0],basis[0]) < 1e-8 || dot(basis[1],basis[1]) < 1e-8 || dot(basis[2],basis[2]) < 1e-8)
        return result;
    const float3 normalVS = normalize(FrameBuffer::WorldToView(normalize(basis[2]) * (front ? 1.0 : -1.0), false));
    result.normal = float4(GBuffer::EncodeNormal(normalVS), 0.0, 1.0);
    result.evidence.z = float(TokenBase + primitive) / 65535.0;
    uint4 color = uint4(round(saturate(input.color) * 255.0));
    result.identityUV = uint4(Instance + 1u, primitive, asuint(input.uv));
    result.colorGradDepth = uint4(color.x | (color.y << 8) | (color.z << 16) | (color.w << 24),
        PackHalf2(dx), PackHalf2(dy), asuint(input.position.z));
    result.basisFlags = uint4(PackOct(basis[0]), PackOct(basis[1]), PackOct(basis[2]),
        (TokenBase + primitive) | (front ? 65536u : 0u) | 131072u);
    return result;
}

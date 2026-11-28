#ifndef NATIVE_INPUT_MRT_HLSLI
#define NATIVE_INPUT_MRT_HLSLI
// Full Native opaque-pass ABI. All four integer attachments use replacement
// writes, never blending. An invalid identity is the integer zero sentinel.
struct NativeInputOutput
{
    float4 Diffuse : SV_Target0;
    float4 MotionVectors : SV_Target1;
    float4 NormalGlossiness : SV_Target2;
    uint4 IdentityUV : SV_Target3;
    uint4 ColorGradDepth : SV_Target4;
    uint4 BasisFlags : SV_Target5;
    uint4 MaterialTechnique : SV_Target6;
    float4 Masks2 : SV_Target7;
};
#if defined(PSHADER)
namespace NativeInputMRT
{
    static const uint AuxLandscape = 1u << 19u;
    static const uint AuxProjection = 1u << 20u;
    static const uint AuxEye = 1u << 21u;
    static const uint AuxFur = 1u << 22u;
    uint PackHalf2(float2 value)
    {
        uint2 bits = f32tof16(clamp(value, -65504.0, 65504.0));
        return bits.x | (bits.y << 16u);
    }
    uint PackOct(float3 n)
    {
        n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-8);
        float2 p = n.xy;
        if (n.z < 0)
            p = (1.0 - abs(p.yx)) * float2(p.x >= 0 ? 1.0 : -1.0, p.y >= 0 ? 1.0 : -1.0);
        uint2 q = uint2(round(saturate(p * 0.5 + 0.5) * 65535.0));
        return q.x | (q.y << 16u);
    }
    float3x3 DerivativeBasis(float3 position, float2 uv)
    {
        float3 px = ddx(position), py = ddy(position);
        float2 ux = ddx(uv), uy = ddy(uv);
        float3 n = cross(px,py);
        float det = ux.x*uy.y-ux.y*uy.x;
        float3 t = px*uy.y-py*ux.y;
        float3 b = py*ux.x-px*uy.x;
        if (abs(det)>1e-8) { t/=det; b/=det; }
        if (dot(n,n)<1e-8) n=float3(0,0,1);
        if (dot(t,t)<1e-8) t=cross(abs(n.z)<0.9 ? float3(0,0,1):float3(0,1,0),n);
        if (dot(b,b)<1e-8) b=cross(n,t);
        return float3x3(normalize(t),normalize(b),normalize(n));
    }
    void Write(inout NativeInputOutput output, float4 position, float2 uv, float4 color,
        float3x3 basis, uint primitive, bool front, uint coverage, float alpha)
    {
        const float2 dx=ddx(uv), dy=ddy(uv);
        const uint instance=Permutation::NativeRasterInstance;
        if (!instance || primitive >= Permutation::NativeRasterPrimitiveCount || (coverage&1u)==0u ||
            !all(isfinite(uv)) || !all(isfinite(dx)) || !all(isfinite(dy)) || !all(isfinite(color)) ||
            !all(isfinite(position)) || !isfinite(alpha) ||
            !all(isfinite(basis[0])) || !all(isfinite(basis[1])) || !all(isfinite(basis[2])) ||
            dot(basis[0],basis[0])<1e-8 || dot(basis[1],basis[1])<1e-8 || dot(basis[2],basis[2])<1e-8 ||
            (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::AdditiveLighting) != 0u)
            return;
        uint4 c=uint4(round(saturate(color)*255.0));
        output.IdentityUV=uint4(instance,primitive,asuint(uv));
        output.ColorGradDepth=uint4(c.x|(c.y<<8u)|(c.z<<16u)|(c.w<<24u),PackHalf2(dx),PackHalf2(dy),asuint(position.z));
        output.BasisFlags=uint4(PackOct(basis[0]),PackOct(basis[1]),PackOct(basis[2]),
            (front?1u:0u)|2u|4u|(uint(round(saturate(alpha)*65535.0))<<3u));
        output.MaterialTechnique=uint4(Permutation::NativeRasterMaterialIndex,Permutation::PixelShaderDescriptor,
            Permutation::ExtraShaderDescriptor,Permutation::ExtraFeatureDescriptor);
        // Binary witness only. The integer draw identity and range table
        // determine visibility and optional scene correspondence.
        output.Masks2.z=1.0;
    }
}
#endif
#endif

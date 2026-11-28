#ifndef NATIVE_HEIGHT_VISIBILITY_HLSLI
#define NATIVE_HEIGHT_VISIBILITY_HLSLI
// Extended Materials soft height shadows: 4 near / 2 far taps. DXR still
// decides geometric visibility first; this only modulates the material response.
float NativeHeightVisibility(NativeParallaxShadowSample sample, float3 light, float viewDistance, float noise)
{
    uint encoded=sample.textureAndMode&65535u;
    if (encoded==0u || encoded>NativeTextureIndexLimit || !all(isfinite(sample.uv)) ||
        !all(isfinite(sample.tangentScale)) || !all(isfinite(sample.bitangentMip)) ||
        !isfinite(sample.height) || sample.tangentScale.w<=0.0) return 1.0;
    uint textureIndex=encoded-1u;
    uint mode=(sample.textureAndMode>>16u)&3u;
    float2 lightTS=float2(dot(normalize(sample.tangentScale.xyz),light),dot(normalize(sample.bitangentMip.xyz),light));
    float2 direction=lightTS*(0.1*sample.tangentScale.w);
    uint taps=viewDistance<1024.0 ? 4u : 2u;
    float accumulated=0.0;
    [loop] for (uint tap=0u;tap<taps;++tap) {
        float4 texel=TexLevel(textureIndex,sample.uv+direction/(float(tap+1u)+noise),mode,sample.bitangentMip.w);
        float height=(sample.textureAndMode & (1u<<19u))!=0u ? texel.a : texel.r;
        if (!isfinite(height)) return 1.0;
        float heightMultiplier=(sample.textureAndMode & (1u<<18u))!=0u ? 0.5 : 1.0;
        accumulated+=max(0.0,saturate(height)*heightMultiplier-sample.height);
    }
    return 1.0-saturate(accumulated*(8.0/float(taps)));
}

#endif

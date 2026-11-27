// Pass-through of a completed native result into the engine's SSR target.
Texture2D<float4> NativeResult : register(t5);
float4 main(float4 position:SV_POSITION):SV_Target0
{
    float4 result = NativeResult.Load(int3(uint2(position.xy),0));
    // Internal dynamic-hit history tag is not part of the engine SSR ABI.
    result.a = saturate(result.a);
    return result;
}

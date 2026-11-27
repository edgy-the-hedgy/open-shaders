// Pass-through of a completed native result into the engine's SSR target.
Texture2D<float4> NativeResult : register(t5);
float4 main(float4 position:SV_POSITION):SV_Target0
{
    return NativeResult.Load(int3(uint2(position.xy),0));
}

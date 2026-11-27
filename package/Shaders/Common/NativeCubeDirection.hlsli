#ifndef NATIVE_CUBE_DIRECTION_HLSLI
#define NATIVE_CUBE_DIRECTION_HLSLI
float3 NativeCubeDirection(uint3 texel, uint size)
{
    float2 p = 2.0 * (float2(texel.xy) + 0.5) / float(size) - 1.0;
    p.y = -p.y;
    float3 d;
    switch (texel.z) {
    case 0: d = float3(1,p.y,-p.x); break;
    case 1: d = float3(-1,p.y,p.x); break;
    case 2: d = float3(p.x,1,-p.y); break;
    case 3: d = float3(p.x,-1,p.y); break;
    case 4: d = float3(p.x,p.y,1); break;
    default: d = float3(-p.x,p.y,-1); break;
    }
    return normalize(d);
}
#endif

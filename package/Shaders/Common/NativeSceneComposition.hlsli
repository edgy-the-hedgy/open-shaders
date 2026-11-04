#ifndef NATIVE_SCENE_COMPOSITION
#define NATIVE_SCENE_COMPOSITION
#ifdef __cplusplus
namespace DX12Native {
#define SCENE_CONSTEXPR constexpr
#else
#define SCENE_CONSTEXPR
#endif
SCENE_CONSTEXPR float NativeSceneChannel(float raster, float direct, float indirect, float hybrid,
    bool directValid, bool indirectValid, bool hybridValid, float blend)
{
    const float base = directValid ? raster + (direct - raster) * blend : raster;
    return base + (indirectValid ? indirect : 0.0f) + (hybridValid ? hybrid : 0.0f);
}
#ifdef __cplusplus
}
#endif
#undef SCENE_CONSTEXPR
#endif

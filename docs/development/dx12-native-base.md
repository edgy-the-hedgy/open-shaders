# DX12 Native Base

This experimental foundation adds an adapter-matched DX12 device, a geometry-free
scene lifecycle, and an opt-in DX12 raster pass-through. The existing DX11 scene,
upscaler selection, post-processing, UI, and presentation path remain authoritative.

## Frame contract

1. Skyrim renders its normal DX11 scene and existing pre-upscale effects.
2. When Native Composite is enabled on a flat world frame, the scene color is
   copied into a DX11/DX12 shared texture and then a DX12-owned texture.
3. A shared GPU fence orders DX11 production before DX12 consumption. The
   published frame has a producer fence and a value for future native consumers.
4. The existing upscaler and all subsequent DX11 passes run unchanged. The
   native frame is diagnostic; it does **not** replace the selected upscaler's
   input in this PR.

Scene transitions pause Native Scene submissions but retain its device/queue.
Native Composite does not depend on scene geometry, RT hardware, or AMD APIs.
All native work is opt-in and falls back to the existing rendering route.

## Scope boundaries

- No geometry preparation, materials, BLAS/TLAS, RayQuery, lighting or shadows.
- No replacement of DLSS/Streamline or FidelityFX dispatch.
- VR retains its original per-eye path; stereo-native capture is not active.
- Post-upscale processing still returns to DX11. DX12-only presentation is a
  separate milestone after native post-processing and UI composition exist.

## Verification

- Build SE, AE and VR and run `cpp_tests` (DXGI headroom arithmetic).
- Compare Native Composite off/on with the same flat scene and upscaler.
- Query `nativeCompositeStatus` and `nativeFrameStatus` through devbench.
- Validate unchanged DLSS, FSR and frame-generation behavior on NVIDIA/AMD.
- Verify VR still renders both eyes without enabling native frame capture.

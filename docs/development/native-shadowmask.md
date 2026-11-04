# Early native RT shadow mask

`DX12NativeScene.NativeShadowMask` enables an independent, full-resolution
directional shadow-mask producer. It defaults off. The menu's early RT shadow
mask checkbox or `setNativeShadowMask` devbench action enables it and requests
prepared scene geometry. `ShadowMaskBudgetMB` defaults to 512 MiB.

The pass first attempts dispatch before Skyrim's raster shadow-map rendering.
It retries at early prepass, world-render begin and pre-opaque prepass when the
current-frame camera or prepared TLAS is not ready. Diagnostics report the actual
dispatch stage and an explicit unavailable reason. A previous-frame camera or
mask is never substituted.

Camera directions are generated from camera matrices without depth, G-buffer or
shadow-map reads. DX12 primary rays find prepared-scene receivers, then secondary
rays test visibility toward the current directional light. Both traversals use
the existing prepared geometry/material bindings, including supported alpha
tests. Unsupported intersected geometry invalidates the pixel conservatively.
This is deterministic hard visibility, with no RR, spatial reconstruction or
temporal denoising requirement.

## Output and future composition

The named `DX12native-shadowmask` resource is RGBA32_FLOAT:

| Channel | Meaning |
| --- | --- |
| R | Directional visibility: 0 blocked, 1 visible; invalid pixels are neutral 1 |
| G | Validity: 1 usable prepared-scene receiver/visibility, 0 unavailable |
| B | Primary receiver ray distance; 0 when invalid |
| A | Prepared TLAS instance index plus one; 0 when invalid |

`GetNativeShadowMask(frame)` borrows the current frame's mask, companion camera-ray
texture, dimensions, exact camera origin and light direction, retained TLAS and
completion fence/value. Availability means submitted, not GPU completion. A
consumer must wait on that GPU fence and transition the resources from COMMON
to its required states, restoring COMMON afterward. It must call
`ReleaseNativeShadowMask(frame, consumerFence, value)` after submitting its work,
before the next frame's producer attempts. The resources remain retained until
that consumer completes. Unused offers expire on the next frame; retaining the
returned metadata alone does not extend the borrowed textures' lifetime.

No image compositor consumes the mask yet. Existing Skyrim raster shadows remain
in the raster source. Later composition must match receivers and apply visibility
to the appropriate directional lighting term, rather than multiplying the combined
scene including ambient, emission and other lights.

## Scope and validation

Coverage is limited to the published prepared TLAS, not the complete dynamic
world. Only the directional light and non-VR main world camera are supported.
Missing light, fresh camera, geometry, bindings, budget or an available frame slot
produces no current mask and leaves other lighting families running. Ray range
is 0.5 to 60000 world units. Material compatibility and visual correctness require
in-game verification by the repository owner.

The camera-direction preparation shader and native DXR shader compile through
their respective FXC/DXC paths. The existing C++ CI target includes a production
contract test covering visibility/validity separation, invalid distances and
pixel-centre coordinates. These checks do not establish GPU dispatch, visual
coverage, image consumption or performance.

Implementation verification on 2026-11-04: Release `CommunityShaders` and
`cpp_tests` compiled; FXC camera-ray preparation and DXC native directional shader
compiled; `CppUtilTests` passed with 571 cases passed, one existing directory
symlink privilege skip and 394061 successful assertions. No deployment, game
launch, GPU capture or runtime acceptance was performed.

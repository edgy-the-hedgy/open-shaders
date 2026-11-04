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

## Output and independent composition

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

The final DX12 compositor can consume the mask as a separately controlled image
layer. Direct illumination, its raw signals, sampling and RR remain unchanged.
As explicitly selected by the repository owner, this layer darkens the final
combined lighting, including ambient, emission and GI. Existing Skyrim raster
shadows remain in the source; this is an additional artistic layer, not replacement
or isolation of the directional lighting term.

| Setting | Default | Range / effect |
| --- | --- | --- |
| `ShadowStrength` | 0 | 0..1 maximum darkening |
| `ShadowBlend` | 1 | 0..1 opacity of the image layer |
| `ShadowRadius` | 0 | 0..32 pixel radius for bounded image-space softening |
| `ShadowReceiverTolerance` | 2 | 0.1..64 world units for receiver matching |
| `ShadowPreview` | false | Grayscale filtered visibility; unmatched receivers magenta |

The `setShadowLayer` devbench command accepts optional `strength`, `blend`,
`radiusPixels`, `receiverTolerance` and `preview` parameters. Enable the producer
first with `setNativeShadowMask`. Strength or blend zero, with preview off, skips
the shadow layer entirely and preserves existing composition behavior. The layer
also works without Direct or Hybrid GI being enabled.

Composition reads the current single-sample depth SRV into an R32 snapshot and reconstructs the
visible world receiver using the current camera. It accepts a mask pixel only
when that position agrees with the prepared primary-ray hit within the selected
tolerance. This late check does not feed depth into the independent RT producer.
The capture shader preserves raw depth values from both standard packed
depth/stencil and reverse-Z float depth/stencil views; it does not copy incompatible
texture formats. Unsupported depth layout/dimensions or insufficient depth-snapshot budget skips
only the layer and records the reason in `nativeFinalScene` diagnostics.

Softening uses at most 25 depth/instance-guided taps. It does not blur validity,
extend coverage across unknown receivers or change the raw shadow mask. Radius
is an image-space control, not a physical light size or new soft-shadow ray model.
`shadowLayerSubmissions` counts submitted composition work, not verified visual
consumption. Both mask textures remain retained through the composition fence.

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
compiled, including the independent image-layer composition shader;
`CppUtilTests` passed with 572 cases passed, one existing directory
symlink privilege skip and 394081 successful assertions. No deployment, game
launch, GPU capture or runtime acceptance was performed.

# Early native RT shadow mask

`DX12NativeScene.NativeShadowMask` enables an independent, full-resolution
prepared-point-light shadow-mask producer. It defaults off. The menu's early RT shadow
mask checkbox or `setNativeShadowMask` devbench action enables it and requests
prepared scene geometry. `ShadowMaskBudgetMB` defaults to 512 MiB.

The pass first attempts dispatch before Skyrim's raster shadow-map rendering.
It retries at early prepass, world-render begin and pre-opaque prepass when the
current-frame camera or prepared TLAS is not ready. Diagnostics report the actual
dispatch stage and an explicit unavailable reason. A previous-frame camera or
mask is never substituted.

Camera directions are generated from camera matrices without depth, G-buffer or
shadow-map reads. DX12 primary rays find prepared-scene receivers, then secondary
rays test visibility toward the selected prepared point lights, terminating at
their emitters. Both traversals use
the existing prepared geometry/material bindings, including supported alpha
tests. Unsupported intersected geometry invalidates the pixel conservatively.
The primary query rejects back-facing triangles as in the original implementation.
Source-material validity, surface normals and the existing prepared-light
eligibility policy are required. Unsupported portal-strict/animated light
baselines remain excluded by the prepared loader.

Visibility is the sum of visible light weights divided by total contributing
light weight. Each weight is light luminance times inverse-square attenuation,
smooth finite-radius cutoff and surface facing. Direct strength, RGB blend and
RR settings do not enter this calculation. No contributing light leaves the
pixel neutral and unavailable, rather than marking it fully shadowed.

## Output and independent composition

The named `DX12native-shadowmask` resource is RGBA32_FLOAT:

| Channel | Meaning |
| --- | --- |
| R | Weighted point-light visibility: 0 blocked, 1 visible; invalid pixels are neutral 1 |
| G | Validity: 1 usable prepared-scene receiver/visibility, 0 unavailable |
| B | Primary receiver ray distance; 0 when invalid |
| A | Prepared TLAS instance index plus one; 0 when invalid |

`GetNativeShadowMask(frame)` borrows the current frame's mask, companion camera-ray
texture, dimensions, exact camera origin, owned prepared light snapshot, retained TLAS and
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
| `ShadowAllLights` | true | Use all prepared lights, bounded to 32 |
| `ShadowLightIndex` | 0 | Select a single light when all-lights is disabled |
| `ShadowStochastic` | true | Sample the emitter disk; tiny smooth source motion affects shadow rays only |
| `ShadowEmitterRadius` | 1 | 0..8 emitter radius multiplier, independent of pixel softening |
| `ShadowTemporal` | true | Accumulate compatible visibility history without RR |
| `ShadowHistoryWeight` | 0.70 | 0..0.95 history blend with current-evidence clamping |

The `setShadowLayer` devbench command accepts optional `strength`, `blend`,
`radiusPixels`, `receiverTolerance`, `preview`, `allLights`, `lightIndex`,
`stochastic`, `emitterRadius`, `temporal` and `historyWeight` parameters. Enable the producer
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

Temporal resolution reprojects the current primary hit through the previous
camera. It requires consecutive frames, unchanged scene and light policy,
matching instance identity and compatible world-space receiver positions.
Visibility history is clamped within 0.18 of current evidence, as in the original
resolve. Invalid current pixels remain invalid; history never invents coverage.
Two bounded slots retain both history and camera uploads through the GPU consumer
fence. Budget or pipeline failure falls back to current raw visibility and records
the temporal failure, without disabling other lighting.

Pixel softening uses at most 25 depth/instance-guided taps. It does not blur validity,
extend coverage across unknown receivers or change the raw shadow mask. Radius
is an image-space control; emitter radius separately controls traced softness.
`shadowLayerSubmissions` counts submitted composition work, not verified visual
consumption. Both mask textures remain retained through the composition fence.

## Scope and validation

Coverage is limited to the published prepared TLAS, not the complete dynamic
world. Only supported prepared static point lights and the non-VR main world camera are supported.
Missing light, fresh camera, geometry, bindings, budget or an available frame slot
produces no current mask and leaves other lighting families running. Ray range
is 0.5 to 60000 world units. Material compatibility and visual correctness require
in-game verification by the repository owner.

The camera-direction preparation shader and native DXR shader compile through
their respective FXC/DXC paths. The existing C++ CI target includes a production
contract test covering visibility/validity separation, invalid distances and
pixel-centre coordinates, weighted light visibility and temporal admission. These checks do not establish GPU dispatch, visual
coverage, image consumption or performance.

The early producer still uses no raster receivers, whereas the original producer
used raster depth to guide and validate its primary queries. The current final-image
layer placement and controls remain unchanged; the original RTComposer mode system
and its shadowed-GI input coupling are not reintroduced. Direct illumination reuses
the same visibility helper with its existing sampling parameters and no added
shadow-only source motion. Its radiance and composition policy remain unchanged.

Verification on 2026-11-04: Release DLL and native DXR/composition/temporal
shaders compiled. `CppUtilTests` passed with 573 cases passed, one existing
directory-symlink privilege skip and 394096 successful assertions. These are
compilation and CPU contract checks; GPU image correctness and performance
remain for repository-owner runtime validation. No geometry schema change or
cache regeneration is required.

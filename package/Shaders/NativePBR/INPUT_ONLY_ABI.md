# NativePBR FXC producer ABI (Chat 3 → Chat 5)

**Status:** existing eight-MRT input-only ABI documented; expanded surface ABI **not yet agreed** with Chat 5. This is a producer handoff, not a claim that DX12 PBR material evaluation is complete.

## Ownership and draw admission

- DX11/FXC still executes the original indexed draw with Skyrim's vertex shader, input layout, interpolation, depth/stencil and all eight deferred MRT bindings. The selected *pixel* shader is `NATIVE_PBR_INPUT`; it does not evaluate TruePBR lighting or material maps.
- Only Lighting/technique `None` with `TRUE_PBR`, `DEFERRED` and opaque static-compatible flags is eligible. Skinned, model-space normal, fur, landscape/LOD, world-map, alpha-tested and character/masked variants keep their original shader.
- Shader availability is resolved before replacing the bound PS. Missing/failed FXC input bytecode, a missing DX12 shader file, absent identity or revision, unexpected MRTs or an exhausted draw table leave the **original indexed draw** in place. They must not trigger a skipped draw.
- An admitted draw writes a frame-local `ordinal` (1–1024) into `MASKS2.r` as `ordinal / 65535` and a signature into `MASKS2.g`: `0xA55A / 65535` for opaque, `0x5AA5 / 65535` for projected. `MASKS2` is `R16G16_UNORM`, so these integers round-trip exactly. Zero is never a material identity.
- A single immutable `SidecarDraw[]` snapshot maps `ordinal - 1` to `{ ordinal, materialIdentity:uint64, materialRevision:uint64, referenceKey, shapeName, projectedUv }`. Ordinals are assigned in indexed-draw execution order and reset for each frame; no TLAS is consulted.
- `MaterialSidecarContract.h` defines the ordinal limit, signatures, packing scale and CPU decode helper. Native consumers must check **the same frame and snapshot** before using an ordinal.

## Current eight-MRT input-only payload

`OpaqueInputs.ps.hlsli` and `ProjectedInputs.ps.hlsli` write all eight existing deferred attachments. The format, binding order and geometry/depth authority are unchanged. The following payload is **diagnostic/intermediate**, not a sufficient SM6.5 PBR surface ABI:

| PS target | Skyrim deferred attachment | Current native-owned pixel data |
| --- | --- | --- |
| 0 `Diffuse` | `MAIN` | Black RGB, alpha 1; DX12 must replace with shaded result |
| 1 `MotionVectors` | `MOTION_VECTOR` | Raster-derived screen-space motion RG |
| 2 `NormalGlossiness` | `NORMALROUGHNESS` (`R10G10B10A2_UNORM`) | Encoded geometric view normal, constant roughness |
| 3 `Albedo` | `ALBEDO` (`R10G10B10A2_UNORM`) | Fractional UV RG, vertex alpha B |
| 4 `Specular` | `SPECULAR` (`R11G11B10_FLOAT`) | Encoded geometric world normal |
| 5 `Reflectance` | `REFLECTANCE` (`R11G11B10_FLOAT`) | Vertex RGB |
| 6 `Masks` | `MASKS` (`R11G11B10_FLOAT`) | Projected direction in `[0,1]`, otherwise zero |
| 7 `Masks2` | `MASKS2` (`R16G16_UNORM`) | Frame-local ordinal + format signature |

The main eight-MRT binding remains intact, and **unowned** pixels retain ordinary DX11 G-buffer semantics. However, native-owned pixel values repurpose some deferred channels. Any DX11 pass that interprets those channels as traditional PBR outputs needs a Chat 5 integration policy; mere MRT-slot compatibility does not prove semantic equivalence. The native composite prototype currently consumes only the partial values above.

## Complete auxiliary sidecar ABI already available (legacy/replay mode)

The separate `MaterialSidecar.ps.hlsl` captures the missing raw inputs **without modifying any deferred MRT**. Its output is post-original-draw replay, with depth-equal testing and a final-depth coverage clear:

| Sidecar slot | Format | Components |
| --- | --- | --- |
| 0 `Identity` | `R32_UINT` | Nonzero draw ordinal |
| 1 `UVAndDdx` | `R32G32B32A32_FLOAT` | UV.xy, ddx(UV.xy) |
| 2 `DdyDepthFacing` | `R32G32B32A32_FLOAT` | ddy(UV.xy), raster depth, front-facing ±1 |
| 3 `Basis0` | `R16G16B16A16_FLOAT` | Interpolated TBN row 0; projected vertex alpha in W |
| 4 `Basis1` | `R16G16B16A16_FLOAT` | Interpolated TBN row 1 |
| 5 `Basis2` | `R16G16B16A16_FLOAT` | Interpolated TBN row 2 (geometric normal) |
| 6 `ProjectedWorldPosition` | `R32G32B32A32_FLOAT` | Projected world position xyz; projected UV z in W |
| 7 `ProjectedDirection` | `R32G32B32A32_FLOAT` | Projection direction xyz; projected UV w in W |

Slots 0–5 cost 60 bytes/pixel and slots 0–7 cost 92 bytes/pixel. The additional projected outputs are present only when projected capture is enabled. `SidecarSurface` and `SidecarTarget` in `MaterialSidecarContract.h` are the matching CPU declarations. The projected path does not yet supply vertex color RGB; this must be agreed if required for the DX12 evaluator.

## Lifetime and synchronization

1. `Deferred::StartDeferred` starts the frame-local producer. `SetupGeometry` records the active material association; the `DrawIndexed` interception commits the ordinal **only** after both FXC PS and eight RTs are confirmed. Shader/constant-buffer/blend state is restored immediately after the original draw.
2. `Deferred::DeferredPasses` ends capture and publishes an immutable draw table before `OnOpaqueRasterReady`. `RasterCapture` copies the unchanged eight DX11 MRT resources into its per-frame wrapped DX12 resources. In the auxiliary mode, it also copies the sidecar targets.
3. DX11 signals a producer fence and flushes; the DX12 queue waits for that value before consuming raster/sidecar resources. The captured frame retains the draw-table shared owner and its texture slots through consumer GPU fences. Never pair a new ordinal texture with a previous frame's table.

## Chat 5 agreement required before expanded integration

FXC/D3D11 cannot simultaneously bind eight deferred RTVs **and additional pixel-shader UAV/RTV surface outputs**: the OM output-slot limit is eight. The existing single-draw input-only packet lacks unwrapped UVs, derivatives, full TBN, facing and projected world inputs, and it does not preserve every traditional DX11 G-buffer semantic on native-owned pixels.

The teams must agree on one of these producer/consumer strategies before claiming the full SM6.5 surface contract:

1. Keep all eight DX11 MRTs semantically compatible and capture complete raw sidecar inputs in a depth-equal second draw (correctness-first; extra raster work).
2. Define a versioned packed eight-MRT **replacement** payload and explicitly update all DX11 consumers of native-owned pixels (single draw; requires a shared input/layout decision and may sacrifice precision).

This branch does not silently switch between those strategies or introduce TLAS/material-recipe readiness gates. Current supported counters: admitted input draws, FXC shader misses, wrong-MRT rejections, draw-table capacity misses, missing identities, and unsupported observed categories.

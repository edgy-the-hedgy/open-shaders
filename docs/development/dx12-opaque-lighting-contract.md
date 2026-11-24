# DX12 opaque lighting publication and ownership

## Execution boundary

The normal DX11 opaque raster path remains complete. `UsesNativeRasterInputs()`
returns false. `OnOpaqueRasterReady` captures depth and the engine camera and
submits NativePBR, selected native direct lighting and the independent shadow
trace. The material publication is independent of diagnostics, lighting and
the selected upscaler.

`Deferred::DeferredPasses` executes SSGI, then SSS. The generic
`OnOpaqueRadianceReady(OpaqueRasterInputs)` hook runs after SSS and before
`DynamicCubemaps::UpdateCubemap`. Its litColor is the existing raster diffuse
radiance at that stage, not final scene color. litSpecular is a separate raster
target. The native scene's `GetOpaqueRasterFrame()`, `GetOpaqueMaterialFrame()`
and `GetOpaqueDirectLightingFrame()` access the open primary opaque pass only.
Check all required publications, including scope, viewport and frame identity.
The borrowed DX11 texture pointers must not be sampled later: enqueue a DX11
read/copy in this callback, retain its output and signal its own producer fence
before a DX12 consumer waits. Do not copy all MRTs indiscriminately.

The existing raster Deferred Composite then runs. After its SRVs/UAVs are
unbound, `OnOpaqueCompositeReady` runs before PostDeferred, Effects11 and
subsequent forward rendering. This is the only native opaque replacement
boundary. Late Native Composite remains responsible for pre-upscale color
capture and the independent raster shadow/diagnostic path; do not overwrite
late scene RGB with opaque lighting.

## Publications

The 32-byte FP16 surface ABI is unchanged. Material status 1 means base
evaluated, 2 means feature-limited; nonpositive statuses are invalid. A depth
match alone is never sufficient to authorize final-color replacement.
`MaterialFrame::correspondence` currently remains DepthOnly.

The existing direct signal is a full-resolution, material-modulated raw
selected-point-light result, with two FP16x4 records per pixel. Diffuse and
specular RGB are independent; alpha >= 1 identifies valid results and encodes
a light count plus one. Its fence, material publication and row pitch are
authoritative. Selected-point-light coverage is not complete direct coverage.

A complete opaque publication uses `DX12Native::OpaqueLightingFrame`:
- `material` is the exact open-pass material publication.
- `allocationOwner` owns the producer allocation; the producer still retains
  it until its own submission retires, even on rejected publication.
- `samples` is a COMMON-state raw buffer with 8 bytes per pixel, FP16x4:
  final linear opaque RGB and a numeric contribution-validity bitmask.
- `readyFence/readyValue` is an actual successfully submitted producer signal
  on the same DX12 device. No CPU fence wait is necessary.
- `temporal` is copied from `material->raster->temporal`.
- `available` describes which contributions the producer actually covers.
- `rowPitch` is the byte pitch; `coverage`, `encoding` and `sampling`
  must be CompleteRasterReplacement, FinalLinearOpaqueColor and FullResolution.

Use the bit constants and `PackOpaqueRadiance` in NativeSurface.hlsli.
The seven required contribution bits are direct diffuse/specular, indirect
diffuse/specular, transmission, emission and remaining opaque effects
(including any fog/atmosphere already baked into the replaced raster result).
A valid zero contribution must have its bit set. Do not mark unimplemented
terms valid. Visibility is a modifier, not an additive radiance signal.

Publish using `DX12NativeScene::PublishOpaqueLighting()` before
`OnOpaqueCompositeReady`. This method and Native Composite both validate
the same-frame contract. The compute shader checks the material status and
the per-pixel validity mask and preserves the original raster RGB whenever
admission fails. The early copy-back is ordered on the DX11 context before
later draws. No full-resolution opaque color allocation is made by this
path until an admissible producer exists.

Independent Shadowmask and whole-pixel replacement are mutually exclusive
for now; this prevents double attenuation of native illumination/emission.
The legacy independent mask still operates on raster scene RGB and is not
a contribution-separated shadow implementation. Keep its behavior unchanged.

## Temporal conventions

RasterFrame retains the engine FrameBuffer (column-vector current jittered
projection/view/inverses, current and previous unjittered view-projection,
camera-relative origins, dynamic resolution parameters). Native depth is raw
engine depth; unproject with the matching inverse projection. reverseDepth
identifies the convention. MotionBlur::GetSSMotionVector returns previous UV
minus current UV, unjittered. Current/previous projection jitter is stored in
render pixels, +x right/+y down, derived from the jittered/unjittered projection
pair. The previous jitter is valid only across a continuous accepted history.

TemporalIdentity distinguishes engine and plugin frame counters, dimensions,
scene scope, geometry and material generation, history revision and reset.
History resets on gaps, engine reset/menu signals, scope/generation/extent
changes, or invalid jitter. This is conservative; independent camera-cut
detection and temporal reconstruction remain future work. Do not add guide
textures until their actual consumer exists.

## Safety and validation

Keep input-only rendering disabled. Do not restore NP15 or direct-only
replacement. The current material producer cannot certify exact raster
identity, and current selected direct lights cannot certify complete direct
illumination. A new indirect producer must not override these gates.
Full Windows, DXIL, FXC, Catch2, SE/AE DevBench and baseline VR validation
remain required.

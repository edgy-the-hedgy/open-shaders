# NativePBR evaluated surface publication (Worker 05 checkpoint)

This is the existing evaluated-surface ABI v1 consumed by Full Native, not a new
raster ABI or a claim that all material techniques have been completed. Worker 03's
raw raster ABI v6 is unchanged. The authoritative producer endpoint is
`NativePBR::FrameProducer::Get(frame)`, which returns the immutable
`DX12Native::NativeScreenProbe::MaterialFrame`. Direct and indirect lighting
already take this publication. New downstream integration (Workers 06, 08, 09,
10 and 12) must consume it or its explicitly derived lighting/reconstruction
publications; it must not independently evaluate primary materials or substitute
TruePBR/raster lighting.

## Evaluated material ABI v1

The source of truth is DX12MaterialFrameContract.h, DX12NativeScreenProbe.h,
NativeSurface.hlsli, NativeMaterialSurface.hlsli and NativePBRProbe.hlsl6.
All offsets below are bytes. Base and lobe vectors are packed IEEE binary16;
no integer identity is encoded in a floating-point channel.

| Buffer | Stride | Layout |
| --- | --- | --- |
| samples | 32 | 0: linear base RGB, material status; 8: world-space shading normal XYZ, roughness; 16: F0 RGB, metallic; 24: emission RGB, material AO |
| rayIdentity | 8 | 0: uint32 TLAS instance; 4: uint32 primitive, bit 31 = raster-derived candidate; UINT32_MAX sentinel invalid |
| materialLobes | 32 | 0: normalRoughness; 8: colorStrength; 16: f0Colored; 24: subsurface; tagged interpretation below |
| optional parallax tail | 32 | 0: float2 displaced UV; 8: uint32 descriptor+1, sampler mode and flags; 12: float height; 16: half4 world tangent and scale; 24: half4 world bitangent and mip |

Each row is padded to eight pixels. Use the published rowPitch,
rayIdentityRowPitch and materialLobeRowPitch, not viewport width times stride.
The optional tail begins after materialLobeRowPitch * height in the same lobe
buffer. The shadow header uses zero as invalid; mode is bits 16-17,
interlayer bit 18, alpha-height bit 19. The lobe ABI is a tagged union, not four
independent lobes: nonnegative f0Colored.w describes coat; -1 thin surface;
-2 standalone fuzz; [-4,-3] explicit thin+fuzz roughness, -5 legacy thin+fuzz;
-6 foliage; [-8,-7] foliage+fuzz roughness, -9 legacy foliage+fuzz. Use
NativeSurface.hlsli accessors, including HasNativeCoat, HasNativeFuzz,
HasNativeThinSurface, HasNativeSubsurface and HasNativeInterlayer.
Do not reinterpret negative tags as a coat parameter.

The frame carries ABI version, frame, dimensions, scope, geometryRevision,
materialRevision, descriptor heap, scene/raster ownership, optional projection,
raw evidence and producer fence/value. Check IsCoherent before binding. Retain
the entire shared_ptr until consumer fence retirement; wait for producer
completion on the consumer queue and transition buffers from COMMON for
consumption, restoring COMMON according to the existing producer/consumer
contract. The current implementation ties material revision to geometry
revision, which is a limitation, not an independently versioned residency ABI.

## Independent validity

Status 1 (EvaluatedBase) means a supported evaluated material; status 2
(FeatureLimited) means usable evaluated base with incomplete technique coverage.
0 is InvalidReceiver; -1 MissingGeometry; -2 DepthMismatch; -3 EvaluationFailed;
-4 UnsupportedMaterial; -5 MissingTextures. HasEvaluatedNativeBase accepts 1
and 2, independently of lighting. Neither status alone grants native final
color ownership. Lighting validity, exact raster winner certification,
contribution completeness and final-color encoding are separate downstream
contracts. Do not synthesize valid material from black output, promote status
2 to 1, or turn absent lighting into a missing material.

The existing ray identity plane is not Worker 03's raster-local draw identity.
Its candidate marker is not an actual traversal hit. The raster input v6 draw
row and local primitive may be valid without any TLAS correspondence. Current
screen evaluation still requires a matching scene instance and geometry; such
raster-local-only surfaces are not yet supported by this publication. A
standalone raster material binding with revision and technique metadata is
needed to eliminate that dependency; do not infer a TLAS mapping.

## Coverage audit (source only)

- Base color, normal, metallic, roughness, AO, emission, F0: shared native
  evaluation exists for resident, admitted materials. This checkpoint adds
  descriptor bounds checks, finite channel validation, and explicit incomplete
  status for unconsumed raw auxiliary techniques.
- Coat, fuzz, thin transmission, subsurface, interlayer: implemented paths for
  supported authored combinations. Not a universal lobe combination contract;
  independent sheen, hair Marschner, glint and other specialized techniques
  are not established by those paths.
- Parallax and height visibility: bounded native UV march and optional shadow
  payload exist; not displaced geometry, universal alpha/displacement parity,
  or evidence of correct execution in every material permutation.
- Projection/snow: projector input and evaluation exist for admitted rows;
  primary projection is intentionally feature-limited. ABI v6 auxiliary
  landscape weights, projection, eye and fur payloads are not yet interpreted
  by this shared material evaluator. Layered landscape, snow and complex
  technique parity remain incomplete.
- Characters: secondary posed-triangle/mover evaluator exists; it is not
  complete primary skin partition, armor, hair, eye or alpha-tested coverage.
  Current separate character material bindings/descriptors require a residency
  audit before claiming one physical texture authority across all paths.
- Alpha-tested geometry: existing native alpha candidate acceptance is limited
  to supported comparison and resident data; raw raster coverage is not
  interchangeable with secondary visibility or displaced cutout parity.
- TXST/MATO: native immutable interpretation and revision-tagged material
  records exist; per-instance and specialized material parity still needs
  validation. The existing material record is 304 bytes and binding is 24 bytes.
- Secondary surfaces: the shared EvaluateMaterialTriangle function is used
  by static and character secondary evaluation; feature-limited secondary
  cases are not silently promoted. Matching primary/secondary appearance,
  mip choice, alpha, projection, character and displacement parity remain
  validation requirements.

No new raster lighting, TruePBR route, sidecar replay, or separate texture
pool is introduced by this checkpoint. This document does not certify that
other existing routes have been retired. Required next work is the raster-local
binding path and auxiliary technique evaluation, complete residency and
material revision separation, followed by Windows DXC SM6.5 compilation,
debug-layer/fence tests and representative in-game materials. Do not mark
Worker 05 complete before those gates are met.

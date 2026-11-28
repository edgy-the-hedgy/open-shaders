# Specialized material v2 integration checkpoint

Architecture approved; source-level producer integration in progress. Runtime switch NOT enabled. Existing Vol2 screen probe, direct/indirect consumers and reconstruction still bind ABI v1. Baseline branch is outside this scope.

## Screen evaluator validation variant

Build target `native_specialized_screen_validation` compiles the real `NativePBRProbe.hlsl6:screenMain` with `NATIVE_SPECIALIZED_V2_PRODUCER=1`. It is not staged in the AIO or loaded by the runtime. No v1 full-resolution specialized pool is written by that variant.

| Binding | Meaning |
|---|---|
| u2 | unchanged primary 32-byte surface |
| u3 | unchanged optional ray identity |
| u4 | independent optional 32-byte parallax-shadow plane, starting at offset zero |
| u5 | 12-byte aligned per-pixel v2 directory |
| u6 | 96-byte sparse v2 payload pool |
| u7 | 17 counter words, reset before each dispatch |
| b2 | payload record capacity |

Directory padding is initialized for the aligned row pitch. Invalid primary receivers publish unknown masks. Ordinary resolved unprojected raster materials can certify absence. Payload overflow retains authored presence, clears unavailable payload validity, and does not invalidate base surface publication. The producer's existing frame status and optional ray evidence remain separate.

The 304-byte recipe record is unchanged in size. Previously reserved controls.y bits 16–22 now expose authored texture roles and bit 23 indicates resolved parameter acquisition; controls.x still records residency. Raster resource admission remains base-first and bounded by the existing descriptor pool. New independent feature-map demand may increase specialized residency pressure. BC4 is rejected for authored packed fuzz roughness because it lacks the green channel.

## Producer coverage

- Generic raster material evaluation: independently accumulates coat, colored coat, coat normal, SSS, thin normal/balance, front/back scattering fractions, specular controls, fuzz and eye normal; unprojected glint parameters are packed.
- Typed grass, recipe grass, and legacy grass: independent available thin/scattering/fuzz data; no invented physical thickness.
- Landscape: records union of authored/evaluated layer presence; specialized blending remains unavailable.
- Interlayer optical depth/thickness, physical transmission thickness, directional hair/anisotropy source data and advanced projection blend remain unimplemented. Packing APIs alone are not evaluation support.
- Diagnostic ray path and character-specific specialized parity require further validation. Current old readiness flags remain as compatibility evidence where the independent recipe metadata is not sufficient.

## Required synchronized runtime change

1. Bind v2 directory/pool/counters and independent parallax storage in NativeScreenProbe, with 20% maximum sparse capacity, actual GetResourceAllocationInfo accounting, optional specialized allocation degradation, reset/barriers, frame version and resource lifetime.
2. Migrate direct and indirect lighting to read independent validity and fields, not to reconstruct a multiplexed NativeCoatSample. Keep front/back scattering, BRDF specular weights, coat energy, fuzz, environment normal, and separate valid zero semantics. Front/back fractions encode the split of the nonopaque share; do not apply opacity/balance again.
3. Coordinate reconstruction history against relevant property validity and stable material identity; sparse ordinal is frame-local and must never be a history key.
4. Stage the v2 shader set and switch all bindings in one coherent runtime configuration. Do not allocate both v1 and v2 full-resolution pools. Base color and upscaling must continue independently.
5. Execute Windows C++ and DXC compilation, then runtime GPU allocation, overflow, performance and material parity tests. Wave reservation is bounded but not starvation-free under sustained saturation.

No runtime GPU or Windows compile results are claimed by this checkpoint.

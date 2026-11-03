# Native compositor: advanced transport modes

This document describes the **implementation**, not a runtime validation claim.
The four upper fixed presets require a completed prepared DX12 scene, material
bindings, DXC-built shaders, and a working native FidelityFX Ray Regeneration
provider. No GPU CPU readback or additional Skyrim draw interception is required.

| Compositor preset | DXR trace pixels/frame | First bounce | FSR RR families | Follow-up |
| --- | --- | --- | --- | --- |
| RT-exclusive Tier I | 25% (rotating 2×2) | 50% survival; diffuse only | Indirect diffuse | None |
| RT-exclusive Tier II | 25% (rotating 2×2) | 50% survival; 50/50 diffuse/GGX | Indirect diffuse, indirect specular | None |
| RT-Enhanced | 50% native checkerboard | 50% survival; 50/50 diffuse/GGX | Indirect diffuse, indirect specular | Post-composite HybridGI at quarter width/height, checkerboard |
| FSR-REDSTONE | 50% native checkerboard | 66% survival; 50/50 diffuse/GGX; 25% conditional second segment | Indirect diffuse, indirect specular | Same post-composite HybridGI |

The original raster scene remains authoritative until a **complete DX12
composition** is ready. The RT-first composition is produced in linear color
for Enhanced and FSR-REDSTONE, followed by the separate HybridGI pass and
one final gamma encode for the DX11 presentation handoff.

## FSR Ray Regeneration selection (RT Composer)

The RT Composer owns an explicit `useFSRRayRegeneration` setting, enabled
by default and editable even while a fixed tier is locked. The setting
does not overwrite the saved legacy/Custom `FSRRayRegeneration.enabled`
preference. A mode change or RR toggle invalidates scene/RR history.

- **Shadow Only** requests the established direct diffuse/specular SDK
  context through the existing D3D11-to-DX12 RR bridge. The fixed tier
  has diffuse-only direct lighting: its specular signal carries no useful
  radiance. The compositor retains the original shadow/raster output if
  direct RR cannot dispatch. Direct radiance is resolved at native size
  before this legacy RR pass, rather than supplying sparse native RR input.
- **Hybrid-Shadowed** uses native DX12 RR for indirect diffuse, indirect
  specular and AO/specular occlusion. It never feeds direct shadow masks
  into an RR radiance signal.
- **RT-exclusive I / II** keep one/two native indirect RR families,
  respectively; no post-composite HybridGI runs.
- **RT-Enhanced / FSR-REDSTONE** keep first-pass native indirect RR plus
  separate post-composite HybridGI RR with diffuse/specular/AO families.
- Turning off the compositor RR switch retains **Shadow Only** fallback
  for fixed modes that require native RR; sparse raw indirect samples
  are never published as if they had been denoised.
- The advanced packer translates internal `-1` *traced miss* to a
  nonnegative far hit distance for AMD, retaining negative alpha only for
  truly unsampled families. Raw transport diagnostics keep their original
  internal markers.
- Native direct diffuse/specular RR, dominant-light visibility RR, and
  FSR Radiance Cache remain separate future integrations. A compositor
  mode name does not imply all seven AMD signals are present.

## GPU ordering

1. DX11 produces the existing scene, depth, normal/roughness, material,
   reflectance and motion inputs and signals the DX12 producer fence.
2. DXR uses the retained TLAS and prepared light snapshot to trace direct
   lighting/shadows and the tier-specific stochastic indirect samples.
3. `nativeAdvancedPack.dxil` packs quarter or checkerboard indirect samples
   into the logical FSR RR grid **without first spatially filling holes**.
   Per-family alpha remains -2 unsampled, -1 sampled miss, nonnegative hit.
4. Native RR validates current depth/normal/motion history. It dispatches
   **one context** for Tier I, **two** for the other advanced first-bounce
   tiers, and **three** for the independent real HybridGI/AO follow-up.
   Neutral deterministic AO/SO never invoke RR in RT-exclusive transport.
5. `nativeHybridCompose.dxil` blends the RT first bounce on DX12. Advanced
   transport is already BRDF-weighted, so primary albedo is *not* multiplied
   a second time.
6. Enhanced modes run post-composite quarter checkerboard HybridGI with
   separate temporal RR context and a final native composition.
7. A fully composed output is exposed to DX11 only after the completion
   fence. Failed stages retain the original raster scene for that frame.

## Runtime and capability gates

- A supported shader model (SM 6.5), native prepared scene, material
  resources, and deployed shader bytecode are prerequisites.
- The four fixed modes require native RR, even if saved Custom RR preferences
  are disabled; failure to initialize RR prevents native publication.
- Memory estimation includes the second GI/RR pipeline for enhanced modes.
  Budget failure retains raster; it does not publish incomplete sparse data.
- Frame discontinuity, scene generation change, extent change, and history
  validation failure reset temporal RR as applicable.
- `FSR Radiance Cache` remains **deferred**. Selecting FSR-REDSTONE does not
  claim that the separate RC model is present.
- The advanced mode path does not yet perform native direct-diffuse/specular
  FSR RR. Direct radiance uses the pre-existing surface-aware DXR direct
  resolve, while native RR is used for indirect families.
- Skinned character geometry still requires its own dynamic BLAS path and
  is not made ray-intersectable by compositor selection.
- Native shader compilation, Windows Release and Skyrim runtime tests
  must be performed on the user's Windows environment.

## Validation checklist

Build `Release` with the `RTDirectLightingDXIL` target available. In Skyrim,
check `advancedTransportDispatches`, `advancedRRDispatches`,
`advancedComposeSubmissions`, `postHybridComposeSubmissions`,
`advancedPresentedFrames` and `advancedRasterFallbackFrames`. With the
selected mode and valid geometry, a successful exclusive frame should
increment presented without post-HybridGI. Enhanced frames should increment
both composition counters before presented.

Test first RT-exclusive Tier I, then Tier II, RT-Enhanced and FSR-REDSTONE.
Repeat on scene change, camera motion/disocclusion, resolution changes,
odd-sized windows (unsupported checkerboard must gracefully fall back), and
VRAM pressure. Verify that failed native RR/context initialization never
publishes sparse raw indirect lighting or corrupts the baseline raster.

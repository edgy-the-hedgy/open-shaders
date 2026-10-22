# FSR Ray Regeneration: RR-only source handoff

## Scope and source identity

- Checkout: `D:\pcmaster\open-shaders-FSRRR`
- Branch: `feat/fsr-ray-regeneration`
- Starting upstream commit: `d66ccad74298929a4cdd07c177ad880f92268b21`
- Source: [PR #987](https://github.com/alandtse/open-shaders/pull/987),
  head `8c11e750899c0e20394b1f327b5a474b87fbabfd`, compared with its
  base `f028ff6a6faa1984d766220c617f8ec0ea1206c6`.
- Delivery is a local source commit. Builds and testing belong to the user.

This port reconstructs SSGI's indirect-specular signal through FidelityFX
Denoiser 1.2. It keeps the existing AO, diffuse GI and ordinary SSGI fallback.
It does not add Radiance Cache, cache training/inference/composition, the RC
provider DLL, Anti-Lag changes, or frame-generation policy changes.

## Connected rendering path

1. `gi.cs.hlsl` writes a current-frame noisy specular signal before ordinary
   SSGI temporal accumulation. RGB contains radiance; alpha contains the
   baseline's radiance-weighted representative view-space hit distance.
2. `rrPrepareInputs.cs.hlsl` prepares linear depth, three-component motion,
   world-space octahedral normals, roughness, and diffuse/specular albedo.
3. SSGI discovers the optional `LightingReconstruction` provider through the
   generic feature interface. Upscaling supplies the FidelityFX backend.
4. `RuntimeDenoiser.cpp` copies inputs into shared DX11/DX12 resources,
   dispatches indirect-specular RR, and orders the output copy with the
   existing shared runtime fence.
5. `rrApplyOutput.cs.hlsl` writes reconstructed RGB into the existing SSGI
   specular output while preserving its visibility alpha. Existing downstream
   SSGI composition then consumes that texture.

## Controls

The `FSR Ray Regeneration` companion tab sits alongside Upscaling controls.
RR requires a loaded Upscaling provider, SE/AE, a recognized Radeon RX 9000
adapter, the denoiser DLL, AO + GI resources selected at boot, Indirect
Lighting enabled, and HQ Specular IL enabled. RR remains enabled by default
as in PR #987; existing GI/specular defaults are preserved.

The existing `openshaders.feature` tool exposes:

- `get` / `set`, `shortName: ScreenSpaceGI`: `EnableRayRegeneration`.
- `runtimeGet` / `runtimeSet`: `RayRegenerationDiagnostics` (default false,
  never persisted).
- `diagnostics`: readiness, inactive reason, current-frame output application,
  depth-history validity, internal dimensions, and nested provider counters/state.

The existing `openshaders.actions` tool exposes, for `ScreenSpaceGI`:

- Command `resetRayRegenerationHistory`, no arguments.
- Query `rayRegenerationStatus`, no arguments.

Selecting AO + GI resources requires a restart if the current boot allocated
AO-only resources. Changing the live RR toggle does not require a restart.
Readiness is an eligibility indication; output-applied diagnostics record
that the current frame queued RR output application, not GPU completion or
visual acceptance.

## Adaptations from PR #987

| Change | Reason |
| --- | --- |
| Removed all RC code, resources, settings, ABI helpers, shaders, packaging and documentation from the extraction | Keep RR independent for this branch; raw SSGI signal goes directly to RR. |
| Omitted the separate reflection-only RR context/API | No source PR caller dispatches it; this port carries the connected SSGI implementation. |
| Kept latest-base Reverse-Z, branding, resource profiles, VR defaults and other unrelated behavior | Avoid replacing newer upstream code with older whole files. |
| Disabled RR in VR at both consumer and provider boundaries | The source uses one full packed-stereo dispatch with eye-zero matrices; a correct per-eye implementation needs separate work. Ordinary VR SSGI remains available. |
| Added an explicit RX 9000 adapter gate | A loaded DLL alone does not establish hardware eligibility. Context creation/dispatch can still fail and fall back. |
| Made RR shader/resource failures optional to ordinary SSGI | The source made RR shaders unconditional dependencies of base SSGI; RR failure should preserve the working path. |
| Reset history after RR gaps, settings/shader/resolution changes and scene transitions | Previous-frame RR state must not survive discontinuities, frame-counter gaps or failed/skipped submissions. |
| Supplied camera jitter scaled to RR's active internal resolution | The source passes zero jitter despite raster-jittered inputs; reuse the existing local FidelityFX jitter convention. Sign and scale are source-derived and still need runtime validation. |
| Matched RR roughness guides to the SSGI producer's existing 0.2 minimum | The feature map must describe the same lobe that produced the signal; producer behavior is preserved. |
| Let RR skip dispatch when the shared command pool is busy | Keep ordinary SSGI output rather than introducing an additional CPU wait for RR. Existing upscaler acquisition behavior is retained. |
| Protected RR teardown with DX12 and DX11 retirement and retained resources on failure | DX12 completion alone does not retire the subsequent DX11 copy; shared-pool resets must not erase RR ownership evidence. |
| Quarantined an RR command context if submission cannot obtain a retirement signal | Prevent the shared pool from resetting an allocator that may still be executing; keep a quarantined slot for the session even across pool resets. |
| Latched RR dispatch exceptions and preserved failure state across DLL reloads | Avoid retrying a provider that has already failed during the session; invalid texture configurations also latch once. |
| Validated RR texture descriptions before shared copies | Reject incompatible extents/formats instead of issuing invalid GPU copies. |
| Added pass instrumentation, frame-stamped runtime diagnostics, provider counters, devbench actions and RR-only UI/tool descriptions | Integrate the port with current repository conventions and expose actual output application separately from eligibility. |
| Kept the target SSGI INI version at `4-8-1` | Do not import the combined PR's `4-9-0` release metadata into this RR-only port. |
| Split allocation and RR rendering into scoped helpers | Keep new functions bounded without refactoring the rest of SSGI. |

No Anti-Lag SDK, input hook, FPS limiter, HDR UI routing, FG menu policy, or
RC provider registration from PR #987 is copied.

## Evidence and remaining validation

Source integration and diff review only. No configuration, compilation,
shader validation, automated test suite, GPU dispatch, game launch, or visual
comparison has been performed for this port. The first source commit is local only; no push of these code changes or PR
has been created.

The pinned denoiser download is registered in `cmake/FidelityFX-Runtime.cmake`;
normal user configuration/packaging retrieves and installs it with Upscaling.
The source PR's minimal Denoiser 1.2 ABI header and required common API types
are carried over without the RC compatibility header.
A user build and in-game testing are still required to establish compiler,
provider, image-quality, temporal-history and fallback behavior.

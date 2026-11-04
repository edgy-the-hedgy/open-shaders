# Native Hybrid GI/AO and native AMD RR delivery

## Delivery and evidence boundary

Branch: `comparison/pre-rtcomposer`. Starting revision: `350b998477ab3377a2fbf84d0756f164650c2913`.

| Stage | Commit | Result |
|---|---|---|
| A | `1c839b3af` | Independent native Hybrid GI/AO |
| B | `f6b8a6831` | Raw signal preparation and coordinate-based packing |
| C | `16b91b8e1` | Native AMD RR with separate signal contexts |
| D | `79595d55e` | PBR material/radiance semantics |
| E | `68e9cb12c` | Independent native composition |
| F | Commit containing this report | Ownership, lifetime, budget and coverage verification |

Compilation and CPU coordinate tests passed. No deployment or game launch occurred. GPU dispatch, visual consumption, SDK runtime behavior and performance remain unverified. Native Hybrid defaults off. Runtime acceptance remains with the repository owner, including a flat edition and VR fallback verification.

## Ownership and dispatch order

Direct remains owned by RTDirectLighting. RT-exclusive indirect remains owned by RTIndirectLighting; selecting it no longer changes the Direct request. RedstoneGI owns independent native screen-space Hybrid GI/AO. Existing feature settings retain sampling ownership. There is no RT Composer or replacement-policy manager.

1. Direct DXR produces raw radiance, material metadata and original coverage. Optional RT-exclusive indirect produces its own raw lobes.
2. Each family proves requested RR coverage, prepares GPU guides/packed inputs and records eligible SDK signals independently.
3. DX12 composes Direct and RT-exclusive indirect into separate HDR contributions, selecting RR per successful lobe and retaining raw otherwise.
4. The existing DX12/DX11 boundary publishes these contributions.
5. The generic `OnDeferredLightingComplete` hook runs after deferred-lighting feature callbacks. Native Hybrid snapshots the resulting scene, produces GI/AO, optionally records its own RR, then composes and publishes its contribution.

Hybrid scheduling requires neither Direct, prepared-scene readiness, DXR geometry nor RR availability. Native execution currently supports the flat main scene target outside map/reflection/transition rendering. Existing VR paths remain available. Simultaneous indirect requests remain selected and are reported by `overlappingIndirectRequests`.

AO/SO attenuate corresponding native GI lobes; they do not independently replace raster ambient lighting. Reduced grids publish exact representative native receivers, without raw neighborhood expansion. SDK checkerboard reconstruction can populate the logical grid; reduced logical grids still publish conservatively.

## Signal contract

| Resource | Format and meaning |
|---|---|
| Raw/packed diffuse and specular | RGBA16F linear radiance, independent validity |
| Native Hybrid AO/SO | R8_UNORM visibility |
| Native Hybrid eligibility / packed validity | R32_UINT |
| RR linear depth | R32_FLOAT, logical grid |
| RR normal, motion, diffuse/specular albedo | RGBA16F, logical grid |
| RR outputs | RGBA16F lobes, R8_UNORM AO/SO |
| Composition | RGBA16F contribution with publication validity alpha |

Direct packed alpha is exactly **0.0 active / -1.0 inactive**, never hit distance. Internal material validity remains separate. Producer metadata is cleared at untraced pixels; the packer checks receiver validity and finite lobes independently. Indirect hit-distance semantics remain separate; invalid packed samples have negative alpha.

Direct emission uses internal metadata channels, is restored at composition and never becomes SDK Direct alpha. Direct SDK lobes are demodulated and material factors restored once. RT-exclusive indirect is demodulated during packing and remodulated when selecting RR. Native Hybrid omits legacy first-bounce material multiplication and saturation transformation in its native permutation, applying material factors and user gains at composition. Existing Direct GGX/Smith/Schlick, roughness, metallic, normal selection and attenuation remain intact. No advanced transport tier was ported.

Motion XY remains engine motion. Camera-derived depth motion is supplied only when the existing 0.5-pixel correspondence criterion agrees; unknown depth motion is zero. Animated/deforming depth motion is not reconstructed. This does not reject radiance or apply an OpenRT history filter.

## Sampling compatibility and fallback

`SignalGrid` separates native/logical dimensions, half-width storage and alternating origin. CPU coverage proof uses the production sampling predicate shared with HLSL and checks requested packed coordinates against actual tracing. GPU packing additionally checks finite data and independent validity. No duplicated rays, promoted reconstructed samples, altered producer pattern or forced Direct resolution is used.

Odd logical widths use populated full-width inputs rather than invalid half-width packing; diagnostics report requested/effective checkerboard. Choices remain selected when incompatible. Compatibility is checked per phase and cached by dimensions, sampling, salt and origin.

The [coverage CSV](native-rr-coverage.csv) records 120 configurations: two dimensions, five Direct modes, stochastic on/off, three RR resolutions and checkerboard on/off. It includes logical/packed dimensions and supported phase counts. All SDK validation entries are `not_run`.

| Producer | RR grids proven across all phases |
|---|---|
| Full Direct, 1920x1080 or 1919x1079, stochastic either | Full/Half/Quarter, full-width or effective checkerboard |
| Fixed Half Direct, 1920x1080 | Quarter RR, full-width or checkerboard |
| Fixed Half Direct, 1919x1079 | Half RR, full-width or checkerboard |
| Other sparse combinations in CSV | Not compatible across every phase; see supported counts |
| Native Hybrid | Producer and packer share their logical/packed grid |
| SDK runtime validated | None; owner testing reserved |

CSV RR modes: 0=Full, 1=Half, 2=Quarter. Direct modes are existing producer values 0..4; the shared predicate is authoritative. This table reports proofs, not a runtime whitelist.

| Condition | Behavior |
|---|---|
| Requested sample not traced | Skip affected RR signals; retain raw coverage |
| One SDK validation/context/configuration/dispatch failure | Skip that signal; other contexts remain eligible |
| Direct layout incompatible | Hybrid diffuse/specular/AO/SO remain eligible |
| RR disabled/provider or optional motion unavailable | Raw production/composition continue |
| Optional RR exceeds owner budget | Retire optional RR; prioritize raw resources |
| Composition shader unavailable | Preserve working raster publication and report failure |
| Native Hybrid shader/resources unavailable | Existing rendering continues |

Direct/indirect diagnostics include trace mode, stochastic state, RR resolution, native/logical/packed sizes, requested/effective checkerboard, origin, frame, requested/recorded masks and exact first untraced native pixel. Each SDK signal independently reports initialization/configuration, failure, queried defaults, history/configuration revision, records and GPU memory. Records/publication counters do not establish visual success.

## Lifetime, budgets and controls

Transport, packing, SDK dispatch and composition stay on GPU. Raw capture retains immutable producer resource references. There is no signal readback or per-frame CPU fence wait. Two frame slots require DX12 completion and DX11 consumer retirement before reuse. Frames retain resources, backend, root signatures, pipelines and descriptors through reload/reset.

Contexts are separate per signal and producer. Actual SDK defaults are queried before applying overrides. Old contexts retire against their last submission fence and are destroyed by nonblocking polling. Extent/layout changes, frame gaps, owner history revisions and configuration changes reset relevant histories.

Admission includes actual native allocations, active/retiring SDK-reported allocations and missing raw-slot reserve. Existing Direct budget defaults to 256 MiB; native Hybrid defaults to 512 MiB, configurable 64..4096. Strict admission can leave RR unavailable until budgets increase. Driver overhead/residency were not measured. Shutdown uses bounded waits; a healthy-device timeout retains in-flight ownership until process exit.

Native Hybrid settings: `NativeHybrid=false`, `NativeBudgetMB=512`, `NativeCheckerboard=false`, `NativeRayRegeneration=true`, `NativeRadianceClamp=false`, `NativeRadianceThreshold=65504`. Hybrid resolution remains independently controlled by RTIndirectLighting. Direct and RT-exclusive RR have independent enable/resolution selections. Shared settings provide requested SDK overrides.

RedstoneGI's `setNativeHybrid` command exposes enabled, RR enable, budget, checkerboard and radiance clamp/threshold with validated bounds. DX12NativeScene diagnostics expose family status, masks, consumption counters and overlap. The legacy DX11 viewer does not mirror native RR textures; named native surfaces support owner RenderDoc inspection. The UI reports this limitation.

Remaining fixed values: two frame slots; existing five Direct modes and block spans 2/4/3; GI minimum linear depth 18; FP16 maximum 65504; existing game-lighting conversion 0.65/exponent 1.6; demodulation floor 0.001; motion agreement 0.5 pixel; composition depth tolerance max(0.5, depth*0.005); existing 32-light/8192-instance bounds.

## Verification

- Universal SE/AE/VR Release `CommunityShaders` built with deployment/archive generation disabled.
- `cpp_tests` built; CTest `CppUtilTests` passed: 561 passed, one skipped, 98,614 assertions passed. The screenshot directory-symlink test skipped because directory-alias support/privilege was unavailable. Existing CI step **Run C++ unit tests** includes these production-code tests.
- DXIL compiled: directional, Hybrid GI/AO, native preparation, Direct composition and Hybrid composition.
- FXC compiled receiver preparation, native input capture and native publication.
- Legacy GI/AO DXBC matched the starting revision byte-for-byte for 24 permutations: GI/AO x Full/Half/Quarter x adaptive on/off x reverse-Z on/off. Local evidence: `build/native-legacy-baseline/verification.csv`.
- Coordinate tests cover exact receivers, origins, odd dimensions, unique mapping, cache agreement and frame overflow. The CSV is generated by production-code tests.
- Source whitespace validation passed. No package/deployment/game/GPU/SDK-runtime/performance validation occurred.

Owner validation must establish actual dispatch/output consumption, inspect packed active-sample provenance and per-signal reasons, toggle producers/RR independently, exercise odd dimensions/resizing/history changes, and check raw fallback plus flat/VR behavior. Compilation and counters do not establish runtime acceptance.

## Exact source inventory

Paths below are relative to the repository root and cover changes from the starting revision. Local generated build evidence, pre-existing scene-prep output and launcher scripts are excluded.

```text
features/RTDirectLighting/Shaders/RTDirectLighting/directLighting.hlsli
features/RTDirectLighting/Shaders/RTDirectLighting/directional.hlsl6
features/RTDirectLighting/Shaders/RTDirectLighting/nativeDirectCompose.hlsl6
features/RTDirectLighting/Shaders/RTDirectLighting/nativeHybridCompose.hlsl6
features/RTDirectLighting/Shaders/RTDirectLighting/nativeSignalPreparation.hlsl6
features/RTDirectLighting/Shaders/RTDirectLighting/prepareReceivers.cs.hlsl
features/RTDirectLighting/Shaders/RTDirectLighting/publishNative.cs.hlsl
features/RedstoneGI/Shaders/RedstoneGI/NativeGICompat.hlsli
features/RedstoneGI/Shaders/RedstoneGI/NativeSignalGrid.hlsli
features/RedstoneGI/Shaders/RedstoneGI/Sampling.hlsli
features/RedstoneGI/Shaders/RedstoneGI/common.hlsli
features/RedstoneGI/Shaders/RedstoneGI/gi.cs.hlsl
features/RedstoneGI/Shaders/RedstoneGI/nativeInputs.cs.hlsl
features/RedstoneGI/Shaders/RedstoneGI/occlusion.cs.hlsl
package/Shaders/Common/NativeTraceSampling.hlsli
src/Deferred.cpp
src/Feature.h
src/Features/DX12NativeScene/DX12NativeScene.cpp
src/Features/DX12NativeScene/DX12NativeScene.h
src/Features/DX12NativeScene/DirectionalDenoiser.cpp
src/Features/DX12NativeScene/DirectionalTrace.cpp
src/Features/DX12NativeScene/DirectionalTrace.h
src/Features/DX12NativeScene/NativeCompute.cpp
src/Features/DX12NativeScene/NativeCompute.h
src/Features/DX12NativeScene/NativeDirectComposition.cpp
src/Features/DX12NativeScene/NativeHybridComposition.cpp
src/Features/DX12NativeScene/NativeHybridDispatch.cpp
src/Features/DX12NativeScene/NativeHybridGI.cpp
src/Features/DX12NativeScene/NativeHybridGI.h
src/Features/DX12NativeScene/NativeHybridRR.cpp
src/Features/DX12NativeScene/NativeLightingRR.cpp
src/Features/DX12NativeScene/NativePublication.cpp
src/Features/DX12NativeScene/NativePublication.h
src/Features/DX12NativeScene/PreparedRenderer.cpp
src/Features/DX12NativeScene/RawLightingSignals.cpp
src/Features/DX12NativeScene/SignalGrid.h
src/Features/DX12NativeScene/SignalPreparation.cpp
src/Features/DX12NativeScene/SignalPreparation.h
src/Features/RTDirectLighting/RTDirectLighting.cpp
src/Features/RTDirectLighting/Shaders.cmake
src/Features/RTIndirectLighting/RTIndirectLighting.cpp
src/Features/RTIndirectLighting/RTIndirectLighting.h
src/Features/RedstoneGI/NativeRayRegeneration.cpp
src/Features/RedstoneGI/RedstoneGI.cpp
src/Features/RedstoneGI/RedstoneGI.h
src/Features/RedstoneGI/RuntimeBridge.cpp
src/Features/RedstoneGI/RuntimeConfiguration.cpp
src/Features/RedstoneGI/RuntimeDenoiser.h
src/Features/RedstoneGI/Session.cpp
src/Features/RedstoneGI/Settings.cpp
tests/cpp/CMakeLists.txt
tests/cpp/test_signal_grid.cpp
docs/development/native-hybrid-rr.md
docs/development/native-rr-coverage.csv
```

# Native renderer corrections

Starting revision: `74e8407e16d61a49fd38394e61925e9b319d0a67`, branch `comparison/pre-rtcomposer`.
This report supersedes the earlier representative-only publication limitation. Runtime and visual acceptance are reserved for the owner; none is claimed here.

## Stage 1: Direct display reconstruction

`nativeDirectCompose.hlsl6` reads immutable raw Direct inputs and writes only the separate native composition output. Full/valid native samples bypass neighborhood reconstruction. Sparse missing lobes select nearest finite raw samples independently within the producer mode's bounded footprint, rejecting invalid receivers, different room identities and incompatible world-space receiver distances using the baseline constraints. Emission remains separate. No display output is bound to RR preparation. Reduced RR logical output maps across compatible native receiver surfaces instead of publishing only representative pixels. No new display allocation is needed: reconstruction is fused into the existing composition pass, after optional RR preparation.

The surface identity available here is room identity, not an object ID; depth/world-distance rejection remains necessary. Cross-surface rejection can conservatively leave raster fallback at discontinuities. Owner inspection must establish actual visual coverage and boundary quality.

Verification: Release DLL and native composition DXIL compiled; production shared display predicates are exercised by C++ tests. An initial test include-path error was corrected before the successful run. A cached deployment-enabled build was stopped during linking and reconfigured with deployment disabled. No deployment target or runtime test was executed.

## Stage 2: RT-exclusive indirect display

Independent indirect diffuse/specular display selection uses the same bounded, surface-compatible search with its own trace mode. Full deterministic raw output bypasses reconstruction. Each lobe accepts genuine hits or producer misses independently; no Direct validity or RR availability is required for raw display. Indirect strength applies once after per-lobe raw/RR selection. Prepared-scene dependencies are unchanged. RR still reads original raw inputs before composition. Release C++/DXIL and the existing C++ suite are verified for this stage.

## Stage 3: Hybrid display resolution

Native composition now maps every native receiver to logical candidates. A display-only 3x3 logical search selects each lobe and occlusion independently, with depth, decoded-normal, diffuse-material and roughness rejection. Full non-checkerboard grids use only their exact pixel. Checkerboard fallback selects genuine packed samples with matching eligibility; successful SDK signals use reconstructed logical outputs. Raw producer and RR preparation are unchanged. Material factors apply at the destination exactly once. No extra texture or temporal history is introduced.

Production shared coordinate and selection helpers are tested across Full/Half/Quarter, both checkerboard origins and odd/even native sizes. Every planar display pixel finds a genuine raw candidate; tests do not label reconstructed display pixels as traced. Boundary quality and actual frame coverage still require owner validation. Native Hybrid DXIL, Release target and C++ tests passed.

## Stage 4: Initially lit source and accumulation

Source inspection: before deferred composition, MAIN contains opaque raster diffuse/ambient/emission in the engine's diffuse encoding and SPECULAR contains its separate linear specular lighting. The selected legacy provider runs `DrawIndirectLighting`, then deferred composition adds its diffuse/specular indirect and environment terms. Native Direct/RT-exclusive publication follows in feature callbacks; native Hybrid publication follows those callbacks. The old Hybrid capture therefore included those already-composited indirect terms.

The new generic pre-composite hook snapshots MAIN and SPECULAR before `DrawIndirectLighting`. A small DX11 shader uses exactly the deferred compositor's initial diffuse conversion plus linear specular, producing a separate RGBA16F linear source. Native input capture reads that snapshot, never accumulated MAIN. It excludes legacy GI composition, native Direct override and RT-exclusive/native Hybrid publication. It retains raster ambient already present in diffuse. This source is refreshed for the current frame only; missing or stale snapshots explicitly prevent native work. It does not read previous-frame final color. Independent producers remain enabled and their explicitly requested contributions still add at final publication.

The DX11-only snapshot is consumed on the same immediate queue before a later frame can overwrite it. Native per-slot inputs remain independently fenced. Snapshot residency is included in native GI accounting. Diagnostics report source name/frame, excluded contributions and the selected legacy provider. This establishes source ordering by inspection, not measured lighting values. New source/input FXC shaders and the complete Release build/C++ suite are verified for this stage.

## Stage 5: Frame-dependent sampling

The actual frame number reaches the native estimator constant buffer. Native sample hashing uses it when checkerboarding or RR temporal sampling is requested; checkerboard origin comes from that same frame number. RR-off, non-checkerboard deterministic operation retains stable noise, including full-resolution operation. No filter or history rejection was added. History reset behavior remains owned by existing relevant layout/configuration/frame-gap revisions. Production phase helpers are covered at ordinary frames and uint32 overflow; native GI/AO DXIL, Release C++ and the test suite are verified.

## Stage 6: Raw ownership and GPU lifetime

Direct and RT-exclusive raw references are published only after all producer dispatches and UAV-to-COMMON visibility barriers. Raw resources belong to the retained frame slot. The diagnostic shadow resolve can no longer write any radiance/material raw UAV, regardless of its export flag. Native Hybrid writes raw outputs only in its estimator dispatches. Preparation reads those resources into distinct guides/packed resources; display composition reads raw/SDK outputs and writes distinct composition resources. The common native pass rejects any input/output or duplicate-output alias before recording barriers. Physical raw copies are unnecessary.

Audit: Direct slots remain owned from DX11 receiver submission through DX12 completion and DX11 retirement; Hybrid slots have the equivalent completion/retirement conditions. Each NativeCompute owner has two descriptor ranges; Direct preparation, indirect preparation, Direct composition, Hybrid preparation and Hybrid composition are separate owners and record once per slot submission. Hybrid estimator GI/AO have separate descriptor ranges within each slot. Frames retain old roots, pipelines and heaps across reload. Resize only reallocates available slots. SDK contexts retire against their own last-use fences; abandoned recordings invalidate history. Failed submitted fences retain the slot/context instead of allowing reuse. Shutdown timeout quarantine remains the conservative exceptional path. No CPU readback or per-frame CPU wait was introduced.

Publication bytecode now reloads with native passes. Initial source capture unbinds an old UAV before binding source SRVs, and checks snapshot budget admission before allocating a new source. Source snapshot bytes and existing native display outputs are included in residency; display reconstruction adds no extra surfaces. Production tests enumerate sparse-mode candidate coordinates across stochastic phases and independent output masks. Incomplete stochastic edge tiles widen only the display search to the adjacent tile; coordinate tests include these edge pixels. If geometry rejection leaves no compatible genuine sample, raster fallback remains. Release/DXIL and C++ tests are verified. Final native sampling changes also preserve 24 recompiled legacy GI/AO DXBC permutations byte-for-byte (`build/native-correction-legacy/verification.csv`).

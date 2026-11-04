# Native renderer corrections

Starting revision: `74e8407e16d61a49fd38394e61925e9b319d0a67`, branch `comparison/pre-rtcomposer`.
This report supersedes the earlier representative-only publication limitation. Runtime and visual acceptance are reserved for the owner; none is claimed here.

## Stage 1: Direct display reconstruction

`nativeDirectCompose.hlsl6` reads immutable raw Direct inputs and writes only the separate native composition output. Full/valid native samples bypass neighborhood reconstruction. Sparse missing lobes select nearest finite raw samples independently within the producer mode's bounded footprint, rejecting invalid receivers, different room identities and incompatible world-space receiver distances using the baseline constraints. Emission remains separate. No display output is bound to RR preparation. Reduced RR logical output maps across compatible native receiver surfaces instead of publishing only representative pixels. No new display allocation is needed: reconstruction is fused into the existing composition pass, after optional RR preparation.

The surface identity available here is room identity, not an object ID; depth/world-distance rejection remains necessary. Cross-surface rejection can conservatively leave raster fallback at discontinuities. Owner inspection must establish actual visual coverage and boundary quality.

Verification: Release DLL and native composition DXIL compiled; production shared display predicates are exercised by C++ tests. An initial test include-path error was corrected before the successful run. A cached deployment-enabled build was stopped during linking and reconfigured with deployment disabled. No deployment target or runtime test was executed.

## Stage 2: RT-exclusive indirect display

Independent indirect diffuse/specular display selection uses the same bounded, surface-compatible search with its own trace mode. Full deterministic raw output bypasses reconstruction. Each lobe accepts genuine hits or producer misses independently; no Direct validity or RR availability is required for raw display. Indirect strength applies once after per-lobe raw/RR selection. Prepared-scene dependencies are unchanged. RR still reads original raw inputs before composition. Release C++/DXIL and the existing C++ suite are verified for this stage.

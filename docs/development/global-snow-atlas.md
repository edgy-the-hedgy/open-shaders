# DX12 Resources: Global Snow Atlas (Milestone 1, incomplete)

The snow atlas is a **global asset inventory**, not a prepared CELL, a scene,
or a snow-rendering feature. Its contents never depend on the latest save,
interior browser selection, streamed subcells, BLAS/TLAS, or DX11 draw hooks.

## Startup and standalone invocation

`DX12 Resources` starts an optional background companion scan after `DataLoaded`
(enabled by default). The user can regenerate/validate it from the main menu.
The companion command is:

```text
DX12ResourcesPrep.exe --snow-atlas <Skyrim Data directory> <Plugins.txt>
```

The scanner walks active BSA indices and loose `meshes`/`textures` assets.
It inspects NIF lighting shader properties and retains their material texture
paths, shader flags, UV transform and sampler clamp values when available.
Snow-named textures also enter the inventory as **candidates**, explicitly not
as proven material matches. Discovery reads no loaded world or CELL records.

## Cache and ownership

```text
Data/SKSE/Plugins/CommunityShaders/DX12-cache/global-snow/
  manifest.json             versioned global material and texture inventory
  textures/<sha256>.dds     unchanged source DDS bytes, deduplicated by content
```

The companion writes content-addressed blobs with temporary names and atomically
replaces the manifest after generation. Failed regeneration leaves the previous
manifest intact. The metadata includes dimensions, mip count, legacy FourCC or
DX10 DXGI format, source origin, original texture path, sampler/UV hints, role
interpretation hints and unresolved reasons. It never downsamples/recompresses
the source DDS. Only `manifest.json` and original DDS bytes persist; a temporary
NIF scan file is removed after the job. Job logs are normally deleted.

`configurationFingerprint` is derived from the enabled plugin listing, INI
content, installed active BSA and plugin file metadata, loose NIF/DDS file
metadata, and auxiliary deployment file metadata. A matching configuration
is reused **only after checking cached DDS SHA-256 contents**. File changes
whose size and modification timestamp are both deliberately preserved are not
detectable for large BSA/plugin/loose assets; stronger archive hash indexing
is required before guaranteeing strict invalidation in that case.

`contentFingerprint` covers deterministic material, texture and candidate
entries and deliberately excludes timestamps, cell identity and resident GPU
pages. It should match when processing different cells with unchanged assets.

## Native interface

`DX12ResourceService::GlobalSnowAtlas::Open` publishes an immutable inventory.
`DX12Resources::GetGlobalSnowAtlas` returns it without consulting the active
scene. `GlobalSnowAtlas::Resolve` maps logical DDS paths to their status and,
when a residency generation is supplied, DX12 page/slice and linear/sRGB SRV
indices. `LoadPages` consumes a bounded requested subset through the existing
`DX12Native::MaterialResources::LoadResidency` allocator, original-mip DDS
decoder, upload fences, page cache, and DXGI budget policy. `MakeDescriptors`
can publish the resulting native page descriptors once upload completion is
confirmed. GPU residency is intentionally independent of the global manifest;
deferred uploads never remove material entries.

Consumers must retain their `DescriptorTable` until their GPU work has
completed; a descriptor index alone does not own the underlying texture page.

## Explicitly incomplete (not Milestone 1 acceptance)

- The scanner does **not yet** reconcile resolved LTEX/TXST snow material IDs,
  effective plugin overrides and all possible custom snow material definitions.
- Runtime projected-snow texture bindings and projection transforms have not
  been proven from a main-menu source. NIF projection fields are recorded, but
  unknown engine projection parameters remain explicitly unresolved.
- BSA files with competing records are **not guessed** into an asset winner;
  duplicate archive paths are rejected until their active ordering is known.
- The DDS writer checks original bytes, SHA-256 and header bounds; full DDS
  decoding, actual DX12 upload, representative texel readback, and shader-visible
  descriptor validation require an installed Skyrim/Windows DirectX runtime.
- No SE/AE or VR main-menu verification has yet been run. The feature itself
  currently reports `coverageComplete=false`, `gpuResidencyVerified=false`.
- This stage does not associate geometry with snow or evaluate snow shading.

Do not mark Milestone 1 complete until these cases have been addressed and the
directive's ten runtime/cache/independence checks have passed. Milestone 2
must consume the global material IDs rather than creating a per-CELL atlas.

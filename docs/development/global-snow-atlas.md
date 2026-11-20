# DX12 Resources: Global Snow Atlas (Milestone 1, incomplete)

The snow atlas is a **global asset inventory**, not a prepared CELL, a scene,
or a snow-rendering feature. Its contents never depend on the latest save,
interior browser selection, streamed subcells, BLAS/TLAS, or DX11 draw hooks.

## Startup and standalone invocation

`DX12 Resources` loads a completed cached atlas after `DataLoaded`, without
launching the companion or rescanning installed assets. The user initiates
generation from the main menu. A second confirmed menu action validates the
cache and regenerates it only if its source configuration has changed or its
cached blobs no longer pass verification.
The companion command is:

```text
DX12ResourcesPrep.exe --snow-atlas <Skyrim Data directory> <Plugins.txt>
```

**ParallaxGen outputs are the sole snow discovery authority.** The companion
requires `Data/ParallaxGen_Diff.json` and `Data/vortex.deployment.json` and
inspects only CRC-validated NIFs tracked by ParallaxGen, plus generated
`meshes/_pgpatcher_dups/` models linked to the same deployed PG package.
Its material candidates come only from snow shader flags or snow texture paths
on those PG NIFs. Snow-named DDS and projected diffuse/normal/noise/detail DDS
outputs directly attributed to that PG package enter as unbound candidates.
No vanilla snow recipe or generic projected-snow texture is added by default.
Files outside PG outputs are not material discovery inputs, even when their
names contain `snow`.

To load textures explicitly referenced by a PG snow material, the companion
checks those exact loose paths and, when required, matching paths in active
BSA filename tables. It does **not** enumerate all loose textures, scan
unrelated NIFs, or extract arbitrary game assets. Discovery reads no loaded
world or CELL records.
Only one generator process may run for a given Skyrim Data directory; a named
Windows mutex rejects concurrent launches. The Skyrim launcher retains its
companion in a kill-on-close Windows Job Object, so closing Skyrim ends an
unfinished scan. The launcher enforces a 30-minute overall bound. Existing
CELL preparation uses the same lifetime guard without a new runtime deadline.

## Cache and ownership

```text
Data/SKSE/Plugins/CommunityShaders/DX12-cache/global-snow/
  manifest.json             versioned PG-only snow material and texture inventory
  textures/<sha256>.dds     unchanged source DDS bytes, deduplicated by content
```

The companion writes content-addressed blobs with temporary names and atomically
replaces the manifest after generation. Failed regeneration leaves the previous
manifest intact. Metadata preserves dimensions, mip count, legacy FourCC or
DX10 DXGI format, resolved texture origin, the PG NIF material source, shader
flags, original texture paths and roles, available UV hints, projected shader
flags and unresolved reasons. DDS source bytes are never downsampled or
recompressed. Generated job logs are normally deleted. Old
`nif-scan-index.json` files from schema v1 are ignored; no global NIF index
is read or written by this generator.

`configurationFingerprint` is derived from PG manifest contents and the PG
output NIF metadata, linked PG snow DDS outputs, active plugin/archive/INI
metadata, and the exact loose DDS paths referenced by PG snow material entries.
No unrelated mesh or loose texture tree enters this fingerprint. A matching
configuration is reused **only after checking each unique cached DDS SHA-256 once**. File changes
whose size and modification timestamp are both deliberately preserved are not
detectable for large BSA/plugin/loose assets; stronger archive hash indexing
is required before guaranteeing strict invalidation in that case.

`contentFingerprint` covers deterministic material, texture and candidate
entries and deliberately excludes timestamps, cell identity and resident GPU
pages. It should match when processing different cells with unchanged assets.

## Native interface

`DX12ResourceService::GlobalSnowAtlas::Open` publishes an immutable inventory.
`DX12Resources::GetGlobalSnowAtlas` returns it without consulting the active
scene. Startup opens only the manifest and cached-file metadata; requested
DDS bytes are SHA-256 checked by the native page loader before upload. The
manual validation/regeneration command performs a full cached-blob check.
`GlobalSnowAtlas::Resolve` maps logical DDS paths to their status and,
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

- No vanilla, non-PG, LTEX/TXST or MATO snow material source is discovered:
  this is an explicit PG-output-only scope decision.
- PG NIF materials with snow shader flags or snow-named texture references are
  classified; PG materials without these indicators are unclassified.
- Projected UV shader flags and available NIF UV hints are preserved, but
  per-material MATO projection parameters and implicit engine texture bindings
  cannot be proven from PG NIFs alone and remain unresolved.
- BSA files with competing records are **not guessed** into an asset winner;
  duplicate archive paths are rejected until their active ordering is known.
- The DDS writer checks original bytes, SHA-256 and header bounds; full DDS
  decoding, actual DX12 upload, representative texel readback, and shader-visible
  descriptor validation require an installed Skyrim/Windows DirectX runtime.
- No SE/AE or VR main-menu verification has yet been run. The feature itself
  currently reports `coverageComplete=false`, `gpuResidencyVerified=false`.
- Regeneration and source/configuration invalidation are currently explicit
  main-menu actions. An automatic invalidation notification has not been implemented.
- This stage does not associate geometry with snow or evaluate snow shading.

Do not mark Milestone 1 complete until these cases have been addressed and the
directive's ten runtime/cache/independence checks have passed. Milestone 2
must consume the global material IDs rather than creating a per-CELL atlas.

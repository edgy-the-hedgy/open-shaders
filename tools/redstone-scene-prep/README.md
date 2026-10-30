# Redstone cell source preparation

Standalone, read-only preparation of one interior's source inputs and rigid opaque meshes. It reads the
deployed PGPatcher output; it does not run PGPatcher or repeat its mesh/material
conflict resolution. It is not connected to ShaderCache or rendering yet.

Requires the .NET 10 SDK. Mutagen.Bethesda.Skyrim is pinned to 0.54.4 and the
managed Nifly reader to 1.1.0, with
transitive dependencies locked in packages.lock.json. The wrapper keeps package,
build and CLI files under the repository's ignored build/scene-prep directory.
It does not build the Skyrim plugin, run tests, deploy or launch the game.

From PowerShell:

```powershell
& .\tools\redstone-scene-prep\PrepareCell.ps1
```

`PrepareAndDeployGuestRoom.bat` is the separate desktop-launcher workflow for
Unslaad.esm:04FD2B. Close Skyrim, then double-click it. It rebuilds only this
standalone tool, regenerates current material/shading/texture source data,
requires all texture sources to resolve, stages/hash-checks the payloads and
deploys the selected room cache. It backs up SettingsUser.json and changes only
the flat RedstoneShadows object to enable prepared geometry, preview and maps;
an existing nonzero view and existing material budget are preserved. Otherwise
it selects source base color and the default 512 MiB material budget. It does
not build the Skyrim plugin, run tests or launch the game. PowerShell 7 is
preferred when installed. Log: `build/scene-prep/prepare-deploy-guestroom.log`.

`PrepareAndDeployJulianosChapel.bat` prepares Glenmoril's **Abandoned Chapel of
Julianos**, `Glenmoril.esm:0099F2` / `zzzGHhqChurchJulianos`, resolved from the
installed Glenmoril.esm CELL record. Close Skyrim before running it. It shares
the same preparation, staging, payload validation and deployment implementation,
but preserves saved runtime settings. The cache goes to
`SKSE/Plugins/CommunityShaders/RedstoneScene/cells/Glenmoril.esm/0099F2` alongside
the guest room. Log: `build/scene-prep/prepare-deploy-Glenmoril.esm-0099F2.log`.
The renderer selects the current interior's cache automatically. This shortcut
has been added in source; its generated geometry and in-game coverage remain
unverified until the user runs preparation and visits the chapel.

Arguments `-DataDirectory`, `-Cell`, `-PluginsFile` and `-OutputDirectory` select
another deployed environment or output location. Cell identity is
`PluginName.esm:hex-local-id`; output record keys use Mutagen's
`hex-local-id:PluginName.esm` representation. Default target is the user-confirmed
Unslaad guest room, `Unslaad.esm:04FD2B` / `zzzCrbKedamaHouse`.

## Inputs and boundaries

The builder also exports `roomSchemaVersion: 1`, stable per-reference room links,
and room volumes with inverse cell transforms and half extents (maximum 128).
These use Skyrim's [XMBR and XMBO record fields](https://raw.githubusercontent.com/Mutagen-Modding/Mutagen/dev/Mutagen.Bethesda.Skyrim/Records/Major%20Records/PlacedObject.xml).
Room data participates in the layout digest. Runtime room indices and generation
stamps are resolved from the current native light table, not stored in the cache.
Native portal-strict light flags are preserved. Unknown, overlapping or
unresolved secondary membership remains unavailable. Existing caches still serve
direct lighting; regenerate both test cells before testing DXR indirect lighting.

The runtime offers exclusive raymarched or DXR indirect lighting. DXR traces
1..8 normalized one-bounce cosine-hemisphere samples per native pixel, evaluating
diffuse and specular lobes from the same samples. Secondary hits use prepared
materials/emission and the represented live point lights, including shadow rays.
This remains a static prepared interior scene; sky misses contribute zero and
unsupported or unvalidated hits do not invoke a raymarch fallback.

Reconstruction controls are saved under `Upscaling.FSRRayRegeneration` and shown
in **Upscaling > FSR-RayRegeneration**. RR resolution and checkerboard packing
are independent of DXR ray count and native trace resolution. Indirect strength
controls brightness without multiplying sample count. All new paths require user
build/deploy, cache regeneration and runtime verification.

- The explicit deployed Plugins.txt supplies enabled plugin order. Installed SE
  official masters and installed entries in the adjacent Skyrim.ccc are included
  before those listings. This is an on-disk baseline, not proof of the currently
  running game's plugin state or Vortex's in-memory active profile.
- Mutagen resolves record identities and winning contexts. Candidate references
  are gathered across CELL overrides, then their winning parent CELL is checked.
  Base-record winners include PGPatcher's generated plugins such as PG_1.esp.
- Only model paths referenced by the selected cell are considered as asset inputs.
  No scan or reclassification of the entire PGPatcher source-asset collection runs.
- ParallaxGen_Diff.json supplies patched CRC32 values. The deployed NIF must match
  before it enters the source cache. PGPatcher-generated duplicate paths can also
  enter when the Vortex deployment manifest associates them with the same output
  package as ParallaxGen_Diff.json; that status is distinct from CRC verification.
- Paths outside that PG output, missing files and CRC mismatches remain explicit.
  There is no general archive fallback or attempt to choose another conflict winner.
- Runtime scripts, save-state changes, injected lights and reference alternate
  textures are not resolved. Mesh compilation considers unconditional,
  enabled STAT, FURN and MSTT references; shape eligibility is separate from record
  eligibility. These records supply baseline geometry, not certified immutable runtime state.

## Outputs

The default output is `build/scene-prep/cells/Unslaad.esm/04FD2B`:

- `cell-index.json`: discovered CELL identities, EditorIDs and containing-plugin
  override chains across the selected load order. It is not a full compiled
  worldspace and does not claim exhaustive runtime scene coverage.
- `cell-records.json`: target placements, raw rotation radians, scale, baseline
  light data, base EditorIDs, source plugins and dependency hashes. Raw reference
  transforms are retained here; composed matrices are in cell-layout.json.
  Static-record classification is not rigid-opaque
  geometry certification.
- `cell-preparation.json`: PG source statuses, cache hits, compilation totals and
  remaining stages. `geometryCompiled` reports partial mesh output;
  `accelerationBuilt` and `runtimeCoverageValid` remain false.
- `source-cache/<sha256>.nif`: unchanged copies of eligible deployed PG inputs,
  deduplicated by SHA256. This is a source cache, not compiled triangles or a BLAS.
- `cell-meshes.json`: shape classifications, source hashes, local bounds,
  shape-to-model transforms and material metadata. Material metadata includes
  original shader flags/type, texture paths, opacity, sidedness and UV transform;
  it is not a complete renderer material representation.
- `cell-layout.json`: admitted shape instances with reference/base identities,
  model-to-cell and shape-to-cell matrices, cell bounds, per-reference/shape
  omissions and four baseline light records for the default target. Matrices
  compose NIF transforms with the CommonLib SetEulerAnglesXYZ record convention;
  they still require runtime comparison. The digest includes compiler identity.
- `cell-preview.html`: self-contained WebGL 2 preview of actual placed triangles.
  Open this generated file in a browser, not the source LayoutPreview.html template.
  Drag to orbit, Shift-drag to pan, wheel to zoom. The height cut exposes interiors.
  Select references by FormKey/base EditorID; omissions include paths and reasons.
  Amber identifies preview-only geometry; the checkbox hides it. Other colors
  identify references; light markers are baseline positions visible through walls.
- `compiled/<compiler-key>/<source-sha256>.json`: cached classifications keyed by
  compiler assembly and reader version. Reuse checks referenced geometry hashes.
- `geometry/<sha256>.rsgm`: little-endian mesh blobs. Four uint32 header values
  contain magic `0x4d475352` (RSGM), version 1, vertex count and index count.
  Float32 XYZ positions follow, then uint32 triangle indices. Positions remain
  shape-local, with original vertex/index ordering and no mesh simplification.
  Shape-to-model transforms are row-major 3x4 matrices for column-vector use.
- `shading/<sha256>.rsga`: optional little-endian shading sidecars, leaving RSGM v1
  unchanged. Four uint32 header values contain magic `0x41475352` (RSGA), version 1,
  vertex count and stride 36. Each vertex contains float3 normal, float2 UV and
  float4 vertex RGBA in the original geometry vertex order. Normals and UVs are
  the reader's shape-local values, without flipping or normalization. RGBA defaults
  to white only when the source explicitly has no vertex colors. Missing normals
  or UVs and invalid counts/non-finite attributes/zero normals produce an explicit
  shading status without excluding the otherwise compiled geometry.

`shadingSchemaVersion` is independent of `rendererSchemaVersion: 1`. New output
uses shading schema 2, which permits both RSGA v1 and v2 blobs. Version 2 has
stride 60: the original 36-byte vertex attributes followed by original float3
tangent and float3 bitangent. Source basis orientation is retained, without
normalization or reconstruction. Counts, finite values and a nonzero basis
determinant are checked. Missing/invalid tangent frames retain v1 shading and an
explicit `tangentStatus`; they do not remove intersection geometry. The native
reader accepts both versions and reports `tangentInstances` separately.

Material schema 1 additionally retains the raw alpha threshold, emissive RGB
and multiplier, specular RGB/strength, glossiness, environment-map scale and
texture clamp mode. Nifly 1.1.0 has no public lighting-shader clamp accessor;
the exporter reads its pinned `_textureClampMode` field and fails explicitly if
that field is unavailable. These remain source parameters, not a declaration of
complete PBR, multilayer or runtime material support.

The producer also discovers optional sibling `<diffuse-stem>_p.dds` parallax
maps, caches their source bytes and records diffuse-to-parallax associations in
`parallaxSources`. Missing optional siblings do not prevent deployment; explicit
NIF texture references retain their existing validation. This does not add
runtime parallax displacement.

`cell-textures.json` records unique texture paths used by represented shapes,
layout identity, content hashes, DDS metadata and source byte counts. The tool
uses deployed loose textures only with an explicit loose-override INI setting;
otherwise it records unresolved precedence. Archive candidates come from the
modeled active plugins and the merged standard Skyrim.ini/SkyrimCustom.ini
archive lists under the user's Documents/My Games/Skyrim Special Edition folder.
Exactly one applicable archive entry is required; ambiguous winners and failed
archive indexing stay explicit. Custom INI/profile mount behavior and runtime
material swaps are not validated. PGPatcher's patched NIF choices are retained;
its conflict resolution is not re-run.

Source DDS files are cached as `textures/<sha256>.dds`, bounded to 128 MiB per
source and 512 MiB per cell (conservative path-byte accounting). Headers and hashes
are checked; `cached` means retained source bytes, not a complete DDS payload or
GPU material validation. Staging verifies hashes and matching cell/layout identity
before copying this optional manifest and its cached sources. Old geometry-only
packages remain stageable. Native texture upload, texture pools, ray-hit sampling,
normal-map transform evaluation and alpha-candidate admission are not enabled by
these source-cache additions.

## Native source-material consumer

RedstoneShadows now has an opt-in `Load prepared material maps` control, an
independent 64..1024 MiB material texture budget (default 512) and ten preview
modes. `setPreparedMaterials` exposes the same controls through devbench: required
`enabled`, optional `view` 0..9 and `textureBudgetMB` 64..1024. Views are visibility,
base color, world normal, source roughness, metalness, emission, material coverage,
material AO, PBR F0 and opacity. Prepared geometry and its preview must also be
enabled. Use a newly regenerated and staged package; the source cache is not
regenerated automatically by the native loader.

The binding worker validates the texture manifest identity and hashes, uses the
existing DirectXTex DDS reader, retains original mip chains and block formats,
and groups matching 2D maps into array pages of at most eight slices. Cubemaps,
arrays, unsupported formats and invalid/budget-exceeding inputs fail explicitly.
GPU destination and staging allocations are checked together. Upload completion
waits occur only on that worker with a five-second bound. Submitted work that
cannot retire retains its GPU resources and latches a trace failure. No upload or
engine-object work is added to per-object render callbacks.

Shared 128-byte material and 32-byte surface records connect instance/triangle
IDs to indices, compiled attributes and pooled maps. Ray hits interpolate the
original UVs/vertex channels, apply UV transforms and sampler addressing, and
sample each map at an explicit mip level from a camera-cone/triangle-area
footprint. Opacity decisions use mip zero. Original normal/tangent/bitangent data
is transformed for model-space or tangent-space normals, including non-uniform
and mirrored transforms. Source PBR channels use the existing on-disk PBR flag,
RMAOS slot/channel conventions and raw NIF roughness/specular scales. Legacy
roughness uses the shared shininess conversion; legacy F0 has no invented PBR
conversion and displays unsupported. Color diagnostics use an explicit linear
sRGB source convention, not the renderer's dynamic working-space pipeline.

Certified opaque blockers retain the opaque path. Material-pending candidates
are accepted only for a supported static, nonblended alpha-tested material with
validated opacity data and source alpha comparison/threshold. Other pending
materials remain excluded. Primary material views are world-space intersections
of prepared triangles; native depth still selects the receiver grid. They are
not complete runtime coverage or final game lighting. Layered PBR, glint,
subsurface/fuzz technique behavior, reference texture/material swaps, texture-set
JSON overrides and dynamic environmental effects are not declared equivalent.
Coverage shows supported source records in green, unavailable data in magenta
and unsupported techniques in orange.

Stable-scene submissions reuse the tables, pool descriptors and geometry views.
Changing only the diagnostic view does not reload textures. Scene changes defer
new texture residency until old material-bearing frames retire. Frame/output
generation checks remain independent of immutable scene-binding reuse. Status
reports trace memory separately from texture residency and table bytes. This
consumer is source-only until separately built and checked in game; it does not
replace native lighting, retire RGI, bypass SCM or submit additional RR signals.

Compiled shapes and placed instances carry `shadingStatus`, `shadingFile` and
`shadingSha256`; placed instances also retain the compiled `material` metadata.
Sidecar hashes are verified before classification-cache reuse and staging.
Older manifests without sidecars continue to stage their v1 geometry. These raw
attributes and material metadata do not establish complete material evaluation
or alpha coverage, and no native renderer behavior changes in this exporter step.

The initial compiler accepts SSE stream-100 BSTriShape and BSSubIndexTriShape
geometry with an unambiguous supported node hierarchy and rigid opaque lighting
materials. BSOrderedNode preserves child transforms and is supported; switch
nodes, hidden nodes, skinning and geometry/transform controllers remain excluded.
Material-only exclusions can retain raw geometry for inspection: alpha-tested,
blended, vertex-alpha and lighting color/float controller surfaces carry
`preview-only:*` status and `opaqueOccluderCandidate=false`. Alpha masks,
transparency and material animation are not reproduced by the flat-color preview.
These surfaces remain listed as omissions from the opaque candidate scene.
Consumers must check the candidate flag; blob existence or a preview instance
does not establish shadow eligibility. This is not full shadow coverage.

The dependency digest excludes timing/cache-hit counters so a warm preparation
keeps the same identity when source inputs are unchanged. Inputs are never written.
Do not distribute cached game/mod assets as part of the tool's source package.

Next stage: compare placements and runtime state before
integrating preparation progress with the shader coordinator and native DX12 scene.

## Native renderer connection

`rendererSchemaVersion: 1` adds explicit per-instance sidedness, material status
and opaque-candidate flags. Stage a freshly prepared cell for local deployment:

```powershell
& .\tools\redstone-scene-prep\StagePreparedCell.ps1 -SourceDirectory .\build\scene-prep\cells\Unslaad.esm\04FD2B
```

This verifies geometry/sidecar hashes and copies only the manifest/referenced blobs
under ignored `build/scene-prep/runtime/SKSE/Plugins/CommunityShaders/RedstoneScene`.
It does not deploy to the game. The local build/deploy wrapper can copy that staged
tree with the DLL and production shader payload when explicitly run.

The native path reads
`Data/SKSE/Plugins/CommunityShaders/RedstoneScene/cells/<plugin>/<local-id>/cell-layout.json`.
Enable **Use prepared cell geometry**, then **Prepared local-light shadow preview**
in RedstoneShadows; select a baseline light. Both settings default off. Equivalent
devbench command: `setPreparedScene` with `enabled`, optional `preview`, and `light`.
Inspect `shadowStatus` for loader, acceleration and actual trace/preview progress.

The loader hashes/validates files on a worker and uploads shared geometry directly
to native DX12 upload buffers. This first connection retains those upload buffers;
it does not yet migrate them into device-local heaps or pool per-preview resources.
All 421 supported guest-room instances can enter BLAS/TLAS preparation; the 89
material-pending instances use a separate instance mask and are excluded from the
initial ray query. The selected baseline point light is traced after world depth
completion, at most once every eight rendered frames, into a diagnostic window.
It does not replace native shadows, feed RR or establish CPU savings. Loading,
epoch invalidation and GPU progress do not add render-thread CPU fence waits.

Plugin/load-order hashes and current object/material/animation state are not yet
validated by this runtime adapter. Matching cell identity plus geometry hashes is
only sufficient for this opt-in diagnostic scope. Production use still requires
state reconciliation, alpha/material evaluation, animation, and lighting consumption.
The plugin, production DXIL and game behavior require separate build/runtime proof.

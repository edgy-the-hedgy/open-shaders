# Redstone cell source preparation

Offline preparation of reusable NIF assets and cell-specific rigid opaque placements. It reads the
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

## Runtime scene revisions (incremental implementation)

Accepted scene packages are retained in a versioned directory after their
TLAS has completed, under
`Data/SKSE/Plugins/CommunityShaders/RedstoneScene/runtime-cache/cells/<plugin>/<cell>/qN/revisions/<layoutDigest>/`.
`qN/active.json` points to the most recent accepted revision. Its publication
is an atomic replacement; previous revisions are intentionally left in place
until garbage collection is implemented. The cache writer executes on a
separate thread, verifies source SHA-256, and hardlinks immutable blobs when
possible. An incomplete revision cannot replace `active.json`.

The native runtime begins with the Q0 cell package (or legacy cell layout),
then looks for `q1/cell-layout.json`, then `q2/cell-layout.json`. Each
candidate is decoded and its BLAS/TLAS built independently. The current scene
stays published until the new scene is complete and its worker fence retires.
Changes to the current stage's package can also trigger a same-tier hot reload.
Failures leave the old scene available. Existing Q1/Q2 package files are
**inputs**; this does not compile new NIFs in the Skyrim process.

Published scene consumers receive a live player identity, cell/worldspace
identity and position if available. This is scene-registration metadata, not
skinned-mesh upload: the player is **not** yet ray-intersectable in the
prepared TLAS. Likewise, detecting an exterior worldspace is not equivalent
to streaming its meshes. Persistent `active.json` is a recovery index; the
runtime checks its source-manifest SHA-256 and can reuse the immutable
revision if the source is unchanged. Corrupt cache metadata or missing cached
assets trigger a fallback to the source prepared package. This checks the
source manifest, NOT the full active mod/load-order fingerprint: the cache
must not be treated as proof that dynamic actor state or plugin overrides
are current. Automatic eviction remains deferred.

**Current limitations:** static interior geometry only; no native worldspace
NIF discovery, mesh deformation, runtime-generated Q1/Q2 assets, cache eviction,
full combined Q0+Q1 VRAM budgeting or verified live plugin-state reconstruction.
The new background upgrade path can temporarily hold two sets of geometry
and acceleration structures in VRAM.

## Q0 baseline and progressive native-scene migration

The default build and stage entry points now operate on **Q0 only**. The Q0-only
compiler reads CRC/SHA-validated deployed NIFs directly, so it no longer
writes an additional 22k-file `source-cache` duplicate. The original deployed
file is revalidated before NIF parsing, protecting against mid-build changes.
Older pre-existing `q0/source-cache` files are not deleted automatically. Legacy
`-QualityLevels` remains accepted for migrations, but separately generated
Q1/Q2 atlases are not part of the new default architecture. Existing Q0
geometry and RSGM/RSGA schemas are backward compatible.

Prepare a Q0-only geometry atlas (offline and outside the game):

    & .\tools\redstone-scene-prep\BuildAtlas.ps1 -QualityLevels 0

Stage and optionally compress the Q0 atlas while Skyrim is closed:

    & .\tools\redstone-scene-prep\StagePreparedAtlas.ps1 -AtlasSourceRoot '.\build\scene-prep\atlas' -QualityLevels 0 -CompressQ0

Or compress an existing Q0 atlas without regenerating its sources:

    & .\tools\redstone-scene-prep\CompressQ0Atlas.ps1 -AtlasRoot '.\build\scene-prep\atlas' -IncludeSourceCache

Compression uses Windows NTFS Compact XPRESS8K, retaining byte-identical
logical RSGM files and their SHA-256 names. The existing DX12 hash verifier
and geometry reader require no changes. Use XPRESS4K to favor read latency,
XPRESS16K/LZX to favor disk space; gains are hardware/content dependent.
This is **disk/cache compression only**; GPU-uploaded position and index data,
BLAS footprints and VRAM residency are unchanged. Compression can take time
on large atlases; it is not performed each time a cell loads.

The DX12 feature now records active cell/worldspace/player form IDs from the
existing frame handoff and invalidates scope when the identity changes.
These IDs are the foundation for future cell/worldspace and dynamic-actor
registration; no additional engine traversal or draw interception is added.

The following work is **not yet implemented** and must not be inferred from
the Q0 changes: native discovery of arbitrary exterior meshes, persistence of
newly assembled runtime cell geometry, asynchronous hot-swapping or disk
overwrite of scene generations, player/NPC skinned geometry in the TLAS,
runtime-produced Q1 shading and Q2 material overlays. The intended migration
is one immutable Q0 geometry catalogue, then incremental Q1/Q2 **overlays**
published only after the new snapshot's GPU fence completes; the current
published snapshot remains available until then. Dynamic actor poses are
live data and must never be reused from a static scene cache.

## Universal asset atlas — offline compiler and native DX12 loader

In-process hostfxr/.NET scene preparation has been retired following runtime crashes.
Skyrim does not launch dotnet or any managed preparator. The .NET 10 SDK is
needed **only for the offline compiler**, not for game runtime.

Generate and deploy the quality-specific atlas with Windows PowerShell 5.1.
Run both commands with Skyrim closed:

    & .\tools\redstone-scene-prep\BuildAtlas.ps1 -DataDirectory 'D:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data'
    & .\tools\redstone-scene-prep\StagePreparedAtlas.ps1 -AtlasSourceRoot '.\build\scene-prep\atlas' -DataDirectory 'D:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data'

The new offline CLI command is: RedstoneScenePrep --atlas DATA OUTPUT_ROOT
QUALITY PLUGINS_TXT (QUALITY = 0, 1 or 2).

### Atlas build performance (incremental)

A legacy compatibility option builds **q0 and q1** using one `--atlas-batch`
pass for both. The default workflow now builds Q0 alone, without q1 preprocessing. Each PGPatcher source NIF is read
and hashed once and compiled into a q1 baseline; q0 reuses its geometry
instead of running a second NIF scan. Use `-QualityLevels 0,1` for a legacy two-tier build or
`-QualityLevels 0,1,2` for the deferred high-storage q2 tier. This does not imply q2
displacement is enabled by default.

The mesh cache now uses an explicitly versioned **semantic compiler key**
instead of the entire .NET assembly SHA. This prevents unrelated changes to
the CLI or UI from invalidating all compiled NIFs. **Bump meshSchema** in
`MeshCompiler.cs` whenever the geometry, shading, material classification or
binary output semantics change. Existing per-NIF blob hashes remain validated;
within one compilation run a verified immutable blob is not rehashed for
every distinct NIF that shares it. When experimental DXR displacement is enabled,
q2 deliberately recompiles targeted meshes instead of reusing their per-NIF
compile cache because referenced DDS content changes are not yet included in
that cache key.

The first run after the compiler-key migration must rebuild old entries;
later runs reuse them if the source SHA and compile parameters match.
For a single tier, `BuildAtlas.ps1 -QualityLevels 1` still calls the
original `--atlas` path. The q0/q1 batch prints source verification,
q1 compilation/cache and q0 derivation times independently so further
bottlenecks can be measured. Q0 and Q1 geometry is shared via hardlinks
on compatible NTFS volumes (copy fallback elsewhere); both names refer to
immutable SHA-256-addressed data. The staging script also prefers hardlinks
for verified atlas blobs. All execution remains outside Skyrim.

 It traverses all PGPatcher-verified
NIFs and deployment-linked generated duplicate NIFs, not only one cell. It
does NOT process arbitrary unpatched/BSA-only meshes, moving runtime state,
or deformable/skinned geometry. Assets are SHA-256-addressed and grouped by
quality under RedstoneScene/atlas/q0, q1 and q2:

- **q0 Base geometry:** reusable triangle blobs; no shading/material sidecars.
- **q1 Shading and material metadata:** vertex attributes and NIF material
  metadata; experimental displacement is not compiled.
- **q2 Full PBR/displacement:** adds cached DDS source data and uses the
  experimental height/normal microgeometry code when explicitly configured.

The catalogue includes atlas-index.json and a per-source NIF compile cache
in compiled/<compiler-key>/<NIF-source-hash>.json. Cell preparation reuses
these entries when compatible with the source content and compiler parameters.

For example, prepare the guest room at q1 into a separate output directory:

    & .\tools\redstone-scene-prep\PrepareCell.ps1 -Quality 1 -OutputDirectory 'D:\pcmaster\openshadersfsrredstone\build\scene-prep\cells\guestroom-q1'

Install each cell quality using StagePreparedCell.ps1 with -Quality 1,
-QualityDirectory, -UseAtlas and -OutputRoot set to the actual Skyrim Data
directory. Install the atlas first. Use distinct per-tier cell output
directories to avoid overwriting a different quality's cell-layout.json.

The native DX12 loader chooses cells/<plugin>/<local-cell-id>/qN if its
manifest exists, or the legacy root cell package as fallback. Missing
geometry, shading and DDS blobs resolve from the atlas of the *same* quality.
The original SHA-256 verification applies to global blobs before GPU upload.
No entire atlas is uploaded to VRAM at once, and GPU BLAS/TLAS are still
built by the existing DXR path.

**Not included yet:** loading-screen blocking, early destination-cell
detection, global asset residency/LRU, general non-PG mesh coverage, dynamic
reference state, skinned/deforming geometry, prebuilt GPU BLAS serialization.
The native runtime does not compile NIFs.

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

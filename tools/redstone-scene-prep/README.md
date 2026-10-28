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

Arguments `-DataDirectory`, `-Cell`, `-PluginsFile` and `-OutputDirectory` select
another deployed environment or output location. Cell identity is
`PluginName.esm:hex-local-id`; output record keys use Mutagen's
`hex-local-id:PluginName.esm` representation. Default target is the user-confirmed
Unslaad guest room, `Unslaad.esm:04FD2B` / `zzzCrbKedamaHouse`.

## Inputs and boundaries

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

This verifies geometry hashes and copies only the manifest/referenced mesh blobs
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

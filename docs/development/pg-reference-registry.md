# DX12 PG Reference Registry (v2)

This registry replaces the global Snow/Ice Atlas generation path. It is a
material-reference index, not a texture atlas, texture cache or CELL database.
Only ParallaxGen/PGPatcher output manifests and their verified deployed NIFs
are eligible for reference discovery. No vanilla content, unrelated NIFs,
arbitrary DDS files or implicit projected-snow bindings are discovered.

## Explicit preparation

In the DX12 Resources main menu, select **Build / Refresh PG Reference Index**
and then **Confirm PG output indexing**. One detached companion invocation
executes:

```text
DX12ResourcesPrep.exe --pg-reference-index <Skyrim Data> <Plugins.txt>
```

The generator obtains a per-installation mutex. It reads `ParallaxGen_Diff.json`
and `vortex.deployment.json`, inspects only their matched PG NIFs, checks the
published patched CRCs, and writes a single versioned metadata index via a
temporary file and atomic replacement. No DDS bytes are copied. The original
texture paths are retained for DX12's existing on-demand Data/BSA loader.

There is no automatic generation, change detection, source fingerprint check,
background refresh, retry loop, or periodic invalidation. Startup reads only
the existing saved index and reports its recorded generation time. Users must
refresh manually after modifying the mod list or PG outputs. A failed refresh
leaves the prior file intact; a failed runtime parse retains the previous
in-memory generation until a valid replacement is available.

## Indexed data

The cache is `Data/SKSE/Plugins/CommunityShaders/DX12-cache/pg-references/index-v2.json`.
The v2 schema interns DDS paths into a shared texture table, deduplicates
material records, and stores mesh paths once with per-shape block indices.

- Mesh: canonical PG output path, SHA-256, source package, shapes.
- Shape: NIF block index, shape name, material index, snow coverage class.
- Material: stable digest, snow/ice/ordinary/ambiguous classification,
  shader flags/type, alpha, nine indexed texture slots, available UV hints.
- Source provenance: PG diff, deployment and Plugins.txt SHA-256 recorded
  *during the confirmed manual run*, never recomputed at startup.

Material classification uses the actual PG shader flags and the main diffuse
and normal texture names. Filename matches deliberately exclude incidental
substrings such as `snowelf` and `apprentice`. Ambiguous classifications are
reported rather than assigned to a snow or ice evaluator.

The native reader owns an immutable CPU-side lookup from canonical mesh path
and NIF block index to material ID, texture references and coverage metadata.
It never enumerates the game directory. Array/page residency, original mips,
async uploads, upload fences and DXGI budgets remain the responsibility of
the existing `DX12Native::MaterialResources` and `NativePBR` path.

## Geometry companion integration

CELL/subcell preparation reads this saved registry instead of reopening the
PG diff and deployment files. The existing companion still resolves placed
geometry, but a mesh must match the indexed PG SHA-256 before its material or
coverage reference is accepted. Missing or modified meshes remain unindexed
until the user explicitly rebuilds the PG registry.

Prepared shape records gain `pgMaterialId`, `pgClassification`, and
`snowCoverage` (`unindexed`, `none`, `native-snow-material`, or
`projected-eligible`). The data follows the immutable scene identity into
native DX12. NativePBR verifies the material and mesh against the current
saved registry before using PG-derived texture paths for snow and ice.

This is indexed *eligibility*, not a baked coverage mask. The exact projected
snow blend, orientation rules, UV transforms and coverage of materials with
no PG evidence require a separate native geometry/evaluator milestone.
In particular, this metadata alone does not make projected snow visually
correct and should not be reported as a completed renderer feature.

## Validation and pending runtime work

The `test_pg_reference_registry.cpp` Catch2 cases cover bounded parsing,
indexed snow/ice lookup, absence of unindexed assets, and rejection of corrupt
indices. Windows build, SE/AE and VR runtime verification remain outstanding.

Next steps: representative PG output fixtures, NativePBR projection evaluation
from companion geometry, verified material readiness for vanilla snow/ice
without RMAOS, and measurements of cold manual indexing versus cached startup.

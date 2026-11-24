# Companion transient tile transfer (RSGT v2)

The persistent Companion process uses an atomic temporary mailbox: request.json
and reply.tile (binary). This is **not** a persistent disk cache. The host owns
and removes the session and result directories. The standalone --tile command
also emits the binary envelope. The host and Companion must be deployed together.

All numeric fields in the binary portion are little-endian. The envelope begins
with uint32 magic 0x54475352 (RSGT), uint32 version 2, uint32 JSON byte length,
then UTF-8 JSON metadata (maximum 16 MiB). It contains placement identities,
transforms, zero-based geometry references, geometryCount, prepared lights, and
coverage/omission metadata. Exactly geometryCount RSGM v1 records follow: uint32
magic 0x4d475352, version 1, vertex count, index count, XYZ float32 vertices,
and uint32 triangle indices. No external file paths are followed by the receiver.

The sender shares decoded arrays across placements and emits each such array
pair once per reply. This is transfer-level deduplication; the current native
scene still owns expanded placed vectors. The existing 48 MiB sender and 64 MiB
receiver expanded geometry budgets remain, along with the 192 MiB transfer
limit. The shape admission limit is 4096 on both sides. Omitted eligible shapes
are recorded in the bounded missing-shape manifest with a count/byte reason.
No PG or persistent cache dependency is introduced by this shared protocol.

## Full Native preparation profile

Mode 2 requests optional RSGA v1/v2 attributes and source materials; it does not
change raster rendering authority. Modes 0/1 continue using transient IPC only.
A profile transition cancels outstanding work and resets the in-memory grid.
In Full Native replies only, each RSGM blob is followed by a uint32 RSGA byte
length and that many bytes. Metadata includes a parallel materials array and
fullNative flag. Invalid shading is not used by the receiver. Source attributes
remain CPU-side; NativePBR and lighting are not implemented by this change.

PG CRC-tracked loose meshes and PG-generated duplicates with verified deployment
provenance are the only sources of material-aware preparation. Other geometry
continues to be admitted through the existing loose/BSA resolver. Vortex reads
vortex.deployment.json. For MO2, set CS_MO2_PROFILE and CS_MO2_MODS (and optionally
CS_MO2_OVERWRITE) in Skyrim's inherited environment; the game and Companion must
see the effective USVFS Data view. Automatic profile discovery is described below; explicit overrides remain supported.

The Full Native cache stores only verified compiled model products keyed by NIF
SHA-256, with Brotli compression, integrity checks, 512 MiB / 8192 entry limits
and oldest-access eviction. No NIF/DDS source copies or per-subcell geometry
packages are retained. The v1 namespace is the compiler/schema revision. The
in-process placement window retains verified complete model bounds; exterior
coverage remains provisional because the origin halo is still bounded.

Full Native uses exact byte-comparison interning of immutable geometry on the
receiver and shares a BLAS for instances referencing the same resident asset.
Weak interning does not pin residency. Existing expanded byte accounting stays
conservative. Lightweight modes keep their existing placed-vector ownership.
The explicit MO2 profile also supplies its plugins.txt to the Companion.

## Automatic mod manager resolution

Full Native selects a deployment context before initializing Mutagen. The host
supplies the runtime full-plugin and light-plugin index sequences. The profile's
active set and each sequence must match; the profile supplies interleaving.
MO2 discovery checks explicit directory overrides, CS_MO2_INI, portable INIs
beside running ModOrganizer.exe instances, and global instance INIs under
LocalAppData. It validates game path, profile order and the physical PG diff
winner against the effective virtual Data bytes. Ambiguity fails closed.
When the game has no observed usvfs_x64.dll module and Vortex deployment exists,
Vortex is preferred (explicit MO2 overrides still take precedence). A running
MO2 process alone is not evidence that this game was launched through MO2.
MO2 file provenance uses enabled mods in descending priority, with overwrite
first. The existing CRC checks remain; untracked PG duplicates must match their
physical winner's bytes. Configuration stamps are checked before every tile;
changes require restarting Skyrim, not reinterpreting the running load order.
No deployment metadata is persisted. Explicit CS_MO2_PROFILE/CS_MO2_MODS and
optional CS_MO2_OVERWRITE remain supported. USVFS inheritance and portable/global
runtime behavior require Windows testing. Archive/profile-local INI changes are
detected, but archive enumeration continues to use the existing Mutagen resolver.

## NativePBR preparation foundation
Full Native placements carry source plugin/local form and NIF shape name. The
compiled cache payload now checks an explicit native-model-v2 marker; previous
entries are replaced lazily within the same bounded cache directory. Completed
TLAS generations own preliminary NativePBR CPU recipes and per-instance binding
metadata. All descriptor and binding indices are invalid until native residency
exists. These recipes do not change TruePBR ownership or final color. The
developer-only Dev tab exposes aggregate scene/resource counters and timing.

Full Native completed TLAS generations may carry a shader-readable geometry
buffer with a 32-byte binding table at byte offset zero, indexed by TLAS
InstanceID. Each record has position, index, attribute byte offsets, attribute
stride, vertex count, index count, position stride, and reserved uint32. The
payload references unique resident geometry and shading once within a generation.
Unavailable records use UINT32_MAX offsets. The upload and TLAS share a producer
fence; the immutable TopLevel retains the buffer through consumer retirement.
Allocation is optional and uses current DXGI headroom; no such buffer is created
in lightweight modes. GPU texture residency and material evaluation are separate.

Full Native material diagnostic: the acceleration worker loads original loose
DDS mip chains from the effective Data view, deduplicates by canonical path,
reuses only fence-completed textures, and creates linear/sRGB descriptor views
(256 individual texture pools, 512 descriptors). It records NativePBRProbe
SM6.5 on the same queue as the scene generation, evaluating one triangle centroid
per eligible instance. The bounded readback is aggregated after the producer
fence and exposed in Dev. This is not screen-space evaluation or authority
transfer: TXST/MATO and live material overrides, archive DDS, and unsupported
formats/features still require integration. The final color remains untouched.

Placement-local alternate TXST and static single-pass MATO identities are
published separately from cached model geometry. TXST matching requires both
shape traversal ordinal and name; ambiguous matches are not evaluated. The
NativePBR catalog owns a separate immutable interpretation of the existing
PBRTextureSets/PBRMaterialObjects JSON schemas. Projected MATO values are kept
in the recipe but are not rendered by the opaque diagnostic. Cache payload v3
includes the shape ordinal and replaces old entries lazily. Deployment and
cache counters are cumulative for the Companion session, transferred as small
optional metadata, and shown exclusively in the Dev tab.

Full Native exterior discovery now indexes the requested worldspace rather than
only the current origin halo. It advances a conservative model-space bounds
catalog in bounded per-request slices, transforms all eight AABB corners, and
admits known overlapping geometry even outside the origin halo. The index is
metadata-only, process-local and never populates the persistent compiled asset
cache. Until indexing finishes, and whenever unsupported sources remain, the
result is partial. Existing tiles are not proactively regenerated by this
index; newly requested tiles use the current index. Lightweight modes retain
the original bounded halo and conservative exterior coverage policy.

Runtime supplements in Full Native can retain generic live shader material and
vertex attributes, independently of TruePBR extension objects. The raster
backend's remapped PBR marker is accepted for diagnostic evaluation only.
Native DDS acquisition prefers loose files and can fall back to the engine's
resource stream for archive-backed assets, with the same bounded decode and
DXGI budget checks. No DDS extraction is persisted. Dev reports archive
residency and separates runtime material omissions from Companion omissions.

The optional Dev visible-pixel diagnostic samples a 64x36 grid of actual raster
pixels at most once per 30 frames. It traces closest native hits against raster
receiver depth, uses committed primitive and barycentric coordinates, and
shares the triangle probe material evaluation. A single fenced submission
retains the immutable scene and raster inputs, with a small readback for counts.
It does not write to the final image or transfer material authority.

The opt-in visible-pixel Dev probe supports native render resolution. It keeps
64-byte material records per pixel on the GPU, in an immutable frame carrying
scope, geometry revision, dimensions and the signaled producer fence. Consumers must wait on that fence. The
shader aggregates five status counters in thread groups; CPU readback is only
20 bytes, regardless of resolution. Allocation is checked against current
DXGI headroom. This still samples once per 30 frames and does not replace
raster material authority or supply final lighting.

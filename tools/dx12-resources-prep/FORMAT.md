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

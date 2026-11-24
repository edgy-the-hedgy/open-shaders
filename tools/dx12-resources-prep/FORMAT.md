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

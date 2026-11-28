# NativePBR specialized material publication v2 — proposed, not approved

Owner: Combined Worker 03 + 05. Reviewers: Workers 06 (direct), 08 (indirect), 09 (reconstruction), lead architect. No GPU-facing consumer contract changes are authorized by this document. Based on Vol2 bac1704e6c6358d9321b7bc538da7718e455e7a0.

## Scope and invariants

Keep raster ABI v6 (eight MRTs, four integer attachments, 80-byte draw table), 32-byte primary material sample, 8-byte optional ray identity and existing producer authority. Specialized availability is independent of raster-local primary validity, geometry correspondence, color publication and native upscaling. Do not restore TruePBR, legacy lighting, or require complete lobes before publishing color.

The existing v1 32-byte NativeCoatSample multiplexes coat, thin, fuzz, subsurface and eye properties using float tags. It cannot express arbitrary combinations. v2 is a separate publication version, not an extension of raster ABI v6. No interpretation of v1 records as v2.

## Exact candidate layout

Per-pixel directory, row-major aligned to eight pixels, 12-byte stride; three uint32 words:
- offset 0: payload ordinal, UINT32_MAX = no record.
- offset 4: present mask (authored or independently detected property).
- offset 8: valid mask (presence or absence established; if present, all fields for that bit are valid).
For every bit: present=0 valid=1 means known absent, present=1 valid=1 means available (zero is legal), present=1 valid=0 means requested but unavailable, present=0 valid=0 means unknown. Unknown and missing never alias absent. Directory initialized for every pixel, including invalid raster receivers; base status is separate.

Bit definitions (proposed): 0 coat, 1 colored coat, 2 coat normal, 3 interlayer, 4 SSS, 5 thin surface, 6 front transmission, 7 back transmission, 8 fuzz, 9 anisotropy, 10 tangent, 11 glint, 12 eye environment normal, 13 transmission color/thickness, 14 front/back specular controls. Remaining bits zero; known absent only when evidence permits. Distinct fields remain readable when another bit is invalid. Dependencies such as coat normal without coat are not an excuse to erase data.

96-byte record, all offsets bytes; little-endian uint32 words. Half pairs low half first; octahedral directions use signed-normalized mapping to two UNORM16 values, decoded and normalized, same convention as raster input. Scene-linear Rec.709 colors, no radiance baked into the material. Directions in the same world space as primary normal; tangent projected onto base normal at consumption. All unused fields initialized to zero; masks, not zero values, determine availability.

| Offset | Encoding | Fields |
|---:|---|---|
| 0 | half2 | coat color R,G |
| 4 | half2 | coat B,strength |
| 8 | oct16x2 | coat normal |
| 12 | half2 | coat roughness, dielectric F0 |
| 16 | half2 | interlayer IOR, layer depth |
| 20 | half2 | interlayer displacement offset, optical thickness |
| 24 | half2 | SSS color R,G |
| 28 | half2 | SSS color B, opacity |
| 32 | oct16x2 | thin-surface geometric bias normal |
| 36 | half2 | subsurface balance, front transmission weight |
| 40 | half2 | back transmission weight, back roughness scale |
| 44 | half2 | front specular weight, back specular weight |
| 48 | half2 | transmission color R,G |
| 52 | half2 | transmission B, thickness |
| 56 | half2 | fuzz color R,G |
| 60 | half2 | fuzz B,strength |
| 64 | half2 | fuzz roughness, signed anisotropy |
| 68 | oct16x2 | tangent orientation |
| 72 | float | glint screen scale |
| 76 | float | glint log microfacet density |
| 80 | float | glint microfacet roughness |
| 84 | float | glint density randomization |
| 88 | oct16x2 | eye environment normal |
| 92 | uint32 | bit 0 foliage, bit 1 tangent handedness negative, remaining reserved |

Field semantics and half-range limits need consumer approval, especially interlayer depth/IOR and front/back weights; no invented defaults. Hair-specific multi-lobe BRDF parameters beyond anisotropy need an explicitly reviewed future extension, not repurposed fields. Glint evaluation remains a consumer responsibility. No new raster attachment.

## Storage alternatives

A: Dense 108 bytes/pixel (12-byte directory + 96-byte payload for all pixels). Predictable coherent indexing, no allocator or overflow; substantial write/read bandwidth and allocation. Not recommended as the default.

B: Supplemental dense planes (existing v1 plus additional independent records). Easy transitional integration but retains conflicting semantics, increases per-frame allocations/descriptors and cannot truly remove mutual exclusion without migrating v1 consumers. Selective per-property sparse pools reduce unused bytes but introduce multiple indices or per-tile indirections, additional descriptor or offset management, more divergent loads and allocation complexity.

C: Recommended compact sparse 12-byte directory plus a bounded pool of 96-byte records. One record per pixel that has any evaluated specialized property, all combinations coexist. Per-group wave/prefix reservation of record ordinals, one atomic per participating wave or group, bounds-check before write. No record when none of the properties need a payload. Overflow leaves base usable and clears validity for present properties whose payload was not written; present bits remain. Counter reset, UAV barrier, publish only after fence, overflow diagnostics per property and frame. Ordinals frame-local and never temporal IDs. One raw SRV/UAV each for directory and pool (or one packed buffer with checked offsets); do not add per-property descriptor tables. Use a bounded allocation counter and ensure no integer overflow; no readback required for execution. For deterministic priority and fairness under saturation, consider tile count/prefix dispatch with rotating tile priority instead of atomics; prototype and measure before approval. Atomic order is not fair by itself.

## Budget (proposal, not measured)

Current producer: 3 submissions, 4 output allocations; width aligned to 8 pixels. Existing parallax-shadow plane adds 32 bytes/pixel when enabled and is unchanged. Base material and ray identity add 40 bytes/pixel, unchanged. Values below are logical MiB across four retained specialized allocations, excluding resource alignment, transient migration duplication, descriptors, raster/base/identity and staging. Proposed starting sparse capacity = floor(0.20 * alignedWidth * height), yielding 31.2 B/pixel average. This is a ceiling on allocated payload, not a claim that only 20% of pixels need lobes. Grass-heavy scenes can exceed it.

| Internal resolution | v1 32B | Sparse v2 20% | Dense v2 | Optional shadow extra |
|---|---:|---:|---:|---:|
| 1920×1080 | 253.1 | 246.8 | 854.3 | 253.1 |
| 2560×1440 | 450.0 | 438.8 | 1518.8 | 450.0 |
| 3840×2160 | 1012.5 | 987.2 | 3417.2 | 1012.5 |

Proposed approval ceiling: initial 32 B/pixel per retained output specialized directory+pool, cap 20% payload slots initially. Permit a separately approved elastic limit up to 60 B/pixel (50% slots) only when actual DXGI allocation budget and the whole in-flight pool allow; full 100% requires 108 B/pixel. Track actual GetResourceAllocationInfo sizes, retained publications, resolution changes, all producer/consumer fences; never allocate both v1 and v2 full-resolution pools simultaneously in normal runtime. At 4K/4 slots 50% is 1898.4 MiB. If the directory/pool cannot fit, publish coherent base and explicit specialized unavailability rather than preventing color/upscaler. No permanent visibility or ray requirements. Budget overflow is degraded coverage, not success. Need recorded specialized occupancy histograms (interior, grass-heavy exterior, character close-up), overflow, allocations, shader wave divergence, GPU timestamps and bytes read/written before raising capacity. Sparse worst-case full occupancy is no cheaper than dense; use group-local coherent allocation and conditional reads. Consumers read directory then only fields for used valid groups. At 60 Hz, logical write traffic is 31.2 bytes/pixel/frame at 20% occupancy, versus 108 dense; actual cache/bandwidth must be measured.

## Versioning and synchronization

Frame-level version, directory stride, payload stride, aligned pitch, allocated record count/capacity, resources/states, base frame stamp, scope, raster material revision, producer fence/value. Reject mismatched dimensions or ABI only for specialized evaluation, never base. v1 and v2 reader selection must be session coherent. Reconstruct each valid signal independently; invalidate specialized history on relevant material/property validity changes; never use payload ordinal as history identity. Separate eye normal from direct lighting normal. Keep parallax shadow independent. Preserve Worker 15 GPU consolidation ownership.

## Required approvals before implementation

- Worker 06: approve offsets/masks, BRDF interpretation, front/back semantics, thin normal/tangent, valid zero, glint/anisotropy and overflow behavior. No mandatory scene.
- Worker 08: approve same layout, coat/refraction, eye normal, fuzz and transmission semantics; independently valid environment contributions.
- Worker 09: approve frame stamp/version, history invalidation and independently reconstructed contributions; no complete-lobe gate.
- Lead architect: approve actual budget (initial 20%, elastic limit if desired), exact field semantics, migration sequence and synchronized consumer integration. Acknowledge no cross-worker edits until approval. Workers 10/12/13 must continue to publish base color/upscaling independently.

All decisions in this document are proposed. No consumer ABI changes, no claimed approval, no runtime validation.

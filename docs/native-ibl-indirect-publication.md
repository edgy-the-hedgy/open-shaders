# NativeIBL indirect publication — Worker 08 checkpoint

Full Native consumes NativePBR evaluated surface ABI v1 (raw raster ABI v6 unchanged) and the completed prior-frame NativeCubemaps environment publication. No legacy IBL or DynamicCubemaps execution, no synthetic EnvironmentFrame, no RTAO/RTGI/sky visibility. Missing environment defers indirect only; material and direct validity are independent. The native feature owns indoor/outdoor indirect even if the legacy IBL toggle is disabled; artistic scalars are inherited. Missing same-frame DALC disables matching for that dispatch, not environment evaluation.

## Worker 09 consumer contract

NativeIBL::Producer::Get(frame) returns a shared immutable IndirectLightingFrame, or null. Match material pointer, frame, scope, geometry/material revisions, dimensions, and quality. Retain the entire publication (including environment, SH and character leases) until consumer fence retirement. Wait for readyFence/readyValue before reading. samples is a raw DX12 buffer, COMMON at the producer boundary; rowPitch is published; 24 bytes per pixel, three IEEE half4 signals:

| Byte offset | Signal | Alpha validity |
| --- | --- | --- |
| 0 | Indirect diffuse, material modulated | ContributionIndirectDiffuse |
| 8 | Indirect specular, material modulated | ContributionIndirectSpecular |
| 16 | Indirect transmission, material modulated | ContributionTransmission |

Zero alpha is absent/unusable; valid zero RGB is not missing. Sampling is Sparse, Raw; use NativeSamplePixel/NativeIsSampled with frame and quality, never unsampled storage. Coverage remains EnvironmentOnly, not CompleteRasterReplacement. Frame availability is a signal mask, not evidence of every pixel or technique. Transmission is independent of direct transmission. Feature-limited material status is not promoted. Primary lobes use the published materialLobeRowPitch, not an assumed material pitch.

SH projects the independently published base and visible exterior sky on DX12. Cache immutable source leases; unchanged cube identity does not reproject. The filtered reflection cube is sampled by perceptual roughness (the producer filters mip i at i/(mips-1)); world outgoing vectors are negated for the incoming cube convention. Exterior full reflection contributes its positive residual over base; absent reflection uses base. Native secondary ray hits blend with the cube miss; no retired 16-bit token evidence is accepted. The existing reconstruction uses environmentContentRevision for strict specular/transmission history, including optional sky/reflection changes; diffuse may evolve within a valid epoch. Scene/encoding/clock epoch changes invalidate SH and history while retired frame allocations can be reused.

## Remaining coverage and validation

The current surface producer still requires valid primary scene correspondence. Armor/character primary pixels participate only where NativePBR actually publishes evaluated material; specialized skin/hair/eye, raster-local-only and unsupported lobe combinations remain unresolved (see native-pbr-surface-publication.md). Character secondary shading uses existing admitted character/mover data. Integer screen-hit correspondence has not been implemented; local native secondary hits are still available with coherent direct-light data. Inferred cubemap texels, missing optional sky/reflection, and unloaded scene geometry are not complete coverage. No runtime validation or DXC/Windows build is claimed by this checkpoint. Verify directional cube orientation with a known bright face, constant and directional SH, indoor armor, exterior sky, roughness/coat/thin surfaces, generation changes, debug-layer resource states and both quality profiles.

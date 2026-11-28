# NativeCubemaps environment publication — Worker 08

Full Native only. NativeCubemaps owns environment acquisition, history, inference and GGX mip filtering. DynamicCubemaps and legacy IBL remain dormant.

## Producer and scheduling

- Native Scene's existing opaque-ready hook submits material/direct/indirect/reconstruction work first; NativeCubemaps' following opaque-ready hook observes the direct diffuse/emission/transmission-only environment-radiance buffer, with same-frame raster evidence. No final scene-color read or DX11 compute lighting is used.
- The first generation can be inferred from same-frame engine DALC, static default DDS and certified engine reflection inputs before reconstruction becomes available. No synthetic observations enter history. Engine reflection and sky acquisition are separate; all six sky faces must be certified and fresh. The shared DX11 capture object provides engine acquisition inputs only.
- DX12 processes one bounded history/base/reflection step per engine frame; all nine mips complete before publication. The last completed generation remains available during processing.
- Native indirect and forward consumers use only a completed generation from an earlier acquisition frame. The DX12 queue may order acquisition after direct/reconstruction submissions but the next generation cannot be consumed by the frame which acquired it. Do not feed final or indirect radiance back into history.

## Consumer API

`NativeCubemaps::Producer::Get()` returns `shared_ptr<const DX12Native::EnvironmentFrame>` or null. The returned generation has:
- stamp: epoch, scope, acquisition engine/plugin frame, encoding, interior and sky-hidden identity; compare with current `EnvironmentStamp::SameEnvironment` (not exact frame equality).
- revision: completed processing fence value; identity is (epoch, revision). Retain the shared pointer until the consumer GPU fence retires.
- base: mandatory filtered 256x256 RGBA16F six-face cube, nine mips, COMMON state at boundaries.
- reflection: optional filtered exterior cube with the same format/mips; absent indoors. Use base if unavailable.
- sky: optional unfiltered, independently stamped engine-acquired sky cube. Reject if hidden/interior, mismatched epoch/scope/encoding, or expired. Its own readyFence/readyValue apply.
- defaultCube: optional immutable static source, not a filtered output. Use base for sampling. It is retained for provenance.
- producerFence/producerValue: nonzero completed DX12 signal; never treat UINT64_MAX as successful completion. Validate each cube independently. Base/filtered reflection age must not exceed 120 engine frames. Null is startup, invalidation, pressure or failure; do not enable legacy IBL as a fallback.
- nativeRadianceInput and rasterCompatibilityInput report admitted source classes, not per-texel coverage. fallbackOnly diagnostics identify an inferred generation without native observations.

## Worker 08 integration

The narrow specular consumer bridge retains a usable generation and samples the roughness-indexed filtered cube before existing RT hit blending. Engine ambient remains the provisional miss only when no valid cube exists. Worker 08 must own diffuse SH projection, sky integration/visibility and settings/normalization. Cache SH by (epoch, source revision, encoding), use the separately stamped sky and retain source leases through consumer retirement. Do not require a same-frame environment revision or wait for an in-flight future generation. No modification of Worker 03's raster ABI is required.

Forward compatibility copies completed native base/reflection mips into DX11 SRVs; water SSR keeps its existing current-frame stencil/cell admission. DX11 samples these published resources but does not perform cube processing. No legacy feature is activated.

## Runtime acceptance

Verify interior and exterior basePublished, exterior reflectionPublished, fresh sky acquisition where visible, nonzero mip resources/revision, forwardEnvironment.published, and nativeRadianceInputAdmitted once raster reconstruction is valid. Check dry-cell water SSR skips, generation rollover, scene transitions, encoding changes and device loss. A successful compile alone does not establish visible coverage.

## Concurrent raster ABI transition

The audited HEAD retires the legacy 16-bit token table. Cubemap observations use the existing RawInputFrame::Matches contract and reconstructed per-pixel candidate/radiance masks, without changing Worker 03's ABI. Opaque and water reflection shaders temporarily receive zero legacy token count: inline ray queries and native secondary lighting remain active, but optional screen-hit radiance reuse is not admitted until its integer identity correspondence is ported. Current-frame water stencil/cell admission is unchanged. The existing architecture lint that requires CaptureRaw's DX11 dispatch is obsolete; Worker 03 owns the shared lint file and must update that assertion (and the older NativeDirect.Load2 assertion) without restoring DX11 lighting.

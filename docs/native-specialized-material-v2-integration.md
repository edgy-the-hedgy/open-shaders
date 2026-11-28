# Specialized material v2 runtime integration checkpoint

The approved ABI is unchanged. Vol2 now selects the v2 screen producer, direct lighting, indirect lighting and reconstruction as its production shader set. This is a source-level runtime migration, not a verified Windows build or in-game result.

## Runtime bindings and storage

| Binding | Producer meaning |
|---|---|
| u2 | unchanged primary 32-byte surface |
| u3 | unchanged optional 8-byte ray identity |
| u4 | independent optional 32-byte parallax-shadow plane, offset zero |
| u5 | 12-byte directory, aligned row pitch |
| u6 | 96-byte sparse payload pool |
| u7 | seventeen counter words, reset before every dispatch |
| b2 | record capacity, directory availability, tile allocation mode |

NativeScreenProbe owns these resources in its existing four retained output slots and three in-flight submissions. Committed output-buffer sizes use GetResourceAllocationInfo. Optional allocation failure degrades to directory-only publication (capacity zero) or an unavailable directory and safe non-addressed backing resources. The base material coherence contract does not require specialized coverage. Tiny backing allocations and the existing per-submission budget reserve still require sufficient memory for submission. No full-resolution v1 specialized buffer is allocated alongside v2. The old materialLobes C++ member now holds only optional parallax shadows or unused backing.

The producer retains the 304-byte recipe, 80-byte raster draw records, eight MRTs, four integer attachments, and raster ABI v6. Directory padding is initialized; invalid receivers publish unknown masks. Overflow retains presence when a directory exists and clears affected availability; a missing directory is explicitly unavailable at frame level. No change to upscaler or presentation ownership.

## Production staging

The normal native composite target builds, stages in AIO, and installs:
- NativePBRScreenProbeV2.dxil
- DirectLightingV2.dxil
- NativeIndirectLightingV2.dxil
- FullNativeReconstructionV2.dxil

C++ loads those exact filenames and their synchronized root signatures; no v1 fullscreen consumer is selected. Old diagnostic/secondary-ray shader source and helper types remain, not a parallel fullscreen publication. Build and install the DLL and all four shaders together.

## Consumer behavior

Direct and indirect lighting decode independently available fields, without synthesizing NativeCoatSample from the directory. Front/back fractions already include their nonopaque share. Coat attenuation, fuzz throughput, thin scattering, subsurface, independent eye reflection normal, directional anisotropy approximation and valid IOR refraction have implementation paths. Missing specialized fields degrade to available base lighting; no requirement for TLAS on primary lighting. Existing native cubemap fallback remains.

Reconstruction compares presence, validity and content, not the transient pool ordinal. Existing stable scene identity, material revisions and motion/depth checks remain in force. Changes affecting only anisotropy, tangent, glint or eye normal can preserve unrelated history; common coupled lobes still invalidate conservatively. Unmapped raster surfaces retain the existing spatial-only reconstruction behavior. Native color authority and emission remain independent.

## Saturation and diagnostics

The maximum capacity remains floor(aligned pixel count / 5). Global wave reservation is the normal path. After two sampled frames at full capacity with at least 125% demand, deterministic 8x8 tile quotas are selected; priorities rotate by 13 lanes every eight frames. The policy returns to global allocation if utilization drops below 75% or demand drops below capacity. Tile quotas deliberately trade some utilization for bounded deterministic local admission. No elastic capacity, extra full-resolution pool, or claimed scene-measured benefit.

Producer diagnostics expose version, directory availability, capacity, actual specialized allocation bytes, sampled requested/allocated/overflow counts, fifteen per-property overflow counters, retained output bytes, tile mode and GPU dispatch milliseconds. Demand means pixels with at least one evaluable present property, not every authored-but-unevaluated property. Samples occur every 120 frames during full-resolution operation. Direct and indirect dispatch timing is likewise sampled every 120 frames using fenced timestamp query pairs; -1 means unavailable. These measure full dispatch time, not isolated lobe execution.

## Validation and remaining work

Portable CPU contract reproduction passed 2,724,864 checks; tile-range simulation passed 263,168 cases. Repository contract tests were extended. Windows C++ compilation, DXC, GPU execution and in-game acceptance remain unverified. Shader CI is configured to include Vol2.

Glint parameters are decoded but stochastic glint evaluation remains outstanding (requires appropriate footprint/noise resources). Interlayer optical depth and transmission thickness acquisition remain incomplete. Anisotropy and layered energy require parity validation; indirect specular uses the front specular control and direct selects by lighting side. Full authored hair/skin/armor/terrain/projection parity remains with the material and BRDF owners. This checkpoint is not a declaration that all specialized acceptance criteria have passed.

Required tests: Windows build and AIO staging; ordinary opaque base without TLAS; forced payload and directory budget failures; dense grass, character close-up, simultaneous lobes, render-resolution changes, sustained saturation, property disappearance and history rejection; independent parallax shadows, native color/upscaling and DX11 post-upscale return. Compare overflow, utilization, timestamp samples and temporal stability before proposing any capacity increase.

# Runtime-only DX12 prototype

This branch uses renderer-observed opaque rigid meshes and SCM-observed point
lights. The runtime never starts the .NET preparation process. The optional
companion build is retained as a reference, off by default.

Enable DX12 and choose Runtime shadow diagnostics. The native mask uses
available geometry; raster shadow ownership remains unchanged. Enable the
diagnostic overlay in Native Scene for a non-authoritative visual comparison
using the existing FSR composition path. The overlay can double-darken shadows.

Light records use observed position, radius, diffuse color and fade. Other
parameters retain native record defaults pending parity work. Only lights
observed through SCM in the current frame are admitted.

Remaining: per-light caster coverage, instance lifecycle, loaded-node
enumeration, GPU-only geometry, adaptive budgets, worker integration and
runtime validation. No unloaded predictive geometry is available.

CPU residency is capped at 2,048 shapes and 256 MiB (1 MiB per shape).
These are provisional fixed diagnostic limits, not measured adaptive budgets.
The worker finishes a coherent same-epoch build before adopting the next
pending revision. A diagnostic scene cannot cross a grid epoch boundary.

Run the existing cpp_tests CTest target for the residency and spatial grid
checks. Record native instance count, CPU geometry bytes, TLAS availability,
native mask output and CPU/GPU timings for baseline and diagnostic mode.

Runtime observations now expire after 600 frames without a recent renderer
witness (swept every 60 frames). This is deliberately conservative: it is
not an engine unload event and may drop off-screen objects. Buffer identity
changes trigger a new observation; in-place edits remain unverified.
BLAS reuse is based on normalized mesh content, independent of placement,
with exact geometry comparison for matching keys. DXGI headroom is checked
before default-heap acceleration allocations; pressure defers builds.
The CPU cap remains fixed. Source and scene expose feature diagnostics.

The scene listens for engine object-loaded events (both load and unload).
Callbacks publish only bounded FormIDs; the render hook invalidates affected
residency and pending packets. This is a first lifecycle mechanism, not proof
of correct handling of every disable, mesh edit or engine event ordering.

An optional reference tool can be staged without a .NET SDK using
BUILD_DX12_RESOURCES_COMPANION=ON, DX12_RESOURCES_PREBUILT_EXE and
DX12_RESOURCES_PREBUILT_SHA256. This does not enable companion execution in
runtime-only mode. A separate repository and its release process are not
created by this build option.

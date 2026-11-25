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

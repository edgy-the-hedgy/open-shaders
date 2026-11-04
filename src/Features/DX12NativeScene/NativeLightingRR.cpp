#include "DirectionalTrace.h"
#include "Features/RTIndirectLighting/RTIndirectLighting.h"
#include "Globals.h"
#include "GpuPass.h"
#include "Utils/RayRegenerationSettings.h"

namespace DX12Native
{
    void DirectionalTrace::RecordNativeLightingRR(Frame& frame)
    {
        CS_GPU_PASS("RTDirectLighting::NativeRayRegeneration");
        frame.nativeDirectRR.recordedMask = frame.nativeIndirectRR.recordedMask = 0;
        const auto shared = RayRegeneration::GetSettings();
        const auto indirectResolution = globals::features::rtIndirectLighting.settings.RRResolution;
        auto record = [&](bool direct, SignalPreparation& preparation, SignalPreparation::Frame& output,
            RedstoneDenoiser& denoiser, SamplingCoverageCache& coverage) {
            const uint32_t requested = (direct ? frame.useDirectRR : frame.useIndirectRR) ? 3u : 0u;
            auto& status = nativeRRStatus[direct ? "direct" : "indirect"];
            status = { { "requestedMask", requested }, { "recordedMask", 0u }, { "frame", frame.number } };
            denoiser.RetireNative(15u & ~requested);
            if (!requested) { output = {}; return; }
            const uint32_t resolution = direct ? frame.directRRResolutionMode : indirectResolution == 3u ? shared.resolutionMode : indirectResolution;
            const auto grid = MakeSignalGrid(frame.size, resolution, shared.checkerboard, frame.number);
            const auto mode = direct ? frame.directTraceMode : frame.indirectTraceMode;
            status = { { "requestedMask", requested }, { "frame", frame.number }, { "traceResolution", mode }, { "rrResolution", resolution },
                { "nativeSize", frame.size }, { "logicalSize", grid.logical }, { "packedSize", grid.packed },
                { "checkerboard", grid.checkerboard }, { "origin", grid.origin }, { "recordedMask", 0u } };
            const auto missing = coverage.Check(grid, mode, frame.stochasticParameters.x >= 0.5f, frame.number, direct ? 0u : 0x51b349d5u);
            if (missing) {
                status["unavailableReason"] = "Requested RR sample was not traced by this producer";
                status["firstUntracedNativePixel"] = *missing;
                denoiser.RetireNative(3u); return;
            }
            if (!frame.motionReady) { status["unavailableReason"] = "Native motion snapshot unavailable"; return; }
            if (!denoiser.InitializeNative(backend)) { status["unavailableReason"] = "Native AMD SDK provider unavailable"; return; }
            const auto offset = direct ? 0u : 5u;
            const std::array inputs{ frame.receivers->resource.get(), frame.rawSignals[2].get(), frame.motion->resource.get(),
                frame.rawSignals[4].get(), frame.rawSignals[3].get(), frame.rawSignals[offset].get(), frame.rawSignals[offset + 1u].get() };
            if (!preparation.Record(backend.d3d12Device.get(), frame.commands.get(), output,
                uint32_t(&frame - frames.data()), grid, inputs, true, direct, false, true, 0.0f, requested)) {
                status["unavailableReason"] = preparation.Failure(); return;
            }
            for (uint32_t i = 0; i < 2; ++i) {
                RedstoneDenoiser::NativeResources resources{};
                for (uint32_t guide = 0; guide < 5; ++guide) resources.guides[guide] = output.guides[guide].get();
                resources.input = output.packed[i].get(); resources.output = output.outputs[i].get();
                resources.nativeSize = frame.size; resources.logicalSize = grid.logical; resources.frame = frame.number;
                resources.origin = grid.origin; resources.checkerboard = grid.checkerboard; resources.direct = direct;
                if (denoiser.RecordNativeSignal(frame.commands.get(), resources, i)) output.recordedMask |= 1u << i;
            }
            status["recordedMask"] = output.recordedMask;
        };
        try { record(true, nativeDirectPreparation, frame.nativeDirectRR, directDenoiser, directCoverage); }
        catch (const std::exception& error) { nativeRRStatus["direct"]["unavailableReason"] = error.what(); }
        try { record(false, nativeIndirectPreparation, frame.nativeIndirectRR, indirectDenoiser, indirectCoverage); }
        catch (const std::exception& error) { nativeRRStatus["indirect"]["unavailableReason"] = error.what(); }
    }
}

#include "NativeHybridGI.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Features/LinearLighting.h"
#include "Globals.h"
#include "GpuPass.h"

namespace DX12Native
{
    uint64_t NativeHybridGI::ResidentBytes() const
    {
        uint64_t bytes = initialRadiance && backend.d3d12Device ?
            NativeTextureBytes(backend.d3d12Device.get(), sourceSize, DXGI_FORMAT_R16G16B16A16_FLOAT) : 0;
        auto* device = backend.d3d12Device.get();
        for (const auto& frame : frames) {
            for (const auto& source : frame.sources) bytes += NativeAllocationBytes(device, source ? source->resource.get() : nullptr);
            for (const auto& output : frame.outputs.textures) bytes += NativeAllocationBytes(device, output.get());
            bytes += NativeAllocationBytes(device, frame.outputs.constants.get());
            bytes += NativeAllocationBytes(device, frame.composed ? frame.composed->resource.get() : nullptr);
            bytes += frame.rr.Bytes(device);
        }
        return bytes;
    }
    void NativeHybridGI::RecordRR(Frame& frame, uint32_t slot, uint32_t number, const RedstoneGI& settings)
    {
        CS_GPU_PASS("RedstoneGI::NativeRayRegeneration");
        frame.rr.recordedMask = 0;
        const uint32_t mask = settings.settings.NativeRayRegeneration && RayRegeneration::GetSettings().enabled ? settings.GetNativeSignalMask() : 0u;
        nativeDenoiser.RetireNative(15u & ~mask);
        if (!mask) { frame.rr = {}; return; }
        try {
            if (!nativeDenoiser.InitializeNative(backend)) { rrFailure = "Native RR SDK unavailable; raw GI/AO retained"; return; }
            if (!frame.motionReady) { rrFailure = "Native RR motion snapshot unavailable within its memory allowance; raw GI/AO retained"; return; }
            const auto budget = uint64_t(settings.settings.NativeBudgetMB) * 1024 * 1024;
            const auto reserve = frame.composed ? 0u : NativeTextureBytes(backend.d3d12Device.get(), frame.size, DXGI_FORMAT_R16G16B16A16_FLOAT);
            uint64_t rawReserve = 0;
            for (const auto& candidate : frames) if (!candidate.allocator)
                rawReserve += EstimateBytes(backend.d3d12Device.get(), frame.outputs.grid.packed[0], frame.outputs.grid.packed[1]) +
                    NativeTextureBytes(backend.d3d12Device.get(), frame.size, DXGI_FORMAT_R32_FLOAT) +
                    5u * NativeTextureBytes(backend.d3d12Device.get(), frame.size, DXGI_FORMAT_R16G16B16A16_FLOAT);
            const auto resident = ResidentBytes() + nativeDenoiser.GetNativeResidentBytes() + reserve + rawReserve - frame.rr.Bytes(backend.d3d12Device.get());
            const auto estimate = SignalPreparation::EstimateBytes(backend.d3d12Device.get(), frame.outputs.grid, mask);
            if (resident > budget || estimate > budget - resident) {
                rrFailure = "Optional native RR surfaces exceed the GI memory budget; raw GI/AO retained";
                nativeDenoiser.RetireNative(mask); frame.rr = {}; return;
            }
            const std::array inputs{ frame.sources[0]->resource.get(), frame.sources[1]->resource.get(), frame.sources[5]->resource.get(),
                frame.sources[3]->resource.get(), frame.sources[4]->resource.get(), frame.outputs.textures[Diffuse].get(), frame.outputs.textures[Specular].get() };
            if (!signalPreparation.Record(backend.d3d12Device.get(), frame.commands.get(), frame.rr, slot, frame.outputs.grid, inputs,
                false, false, true, globals::features::linearLighting.IsLinearLightingActive(), settings.GetNativeGISettings(frame.size, number).SpecularRoughnessFloor, mask)) {
                rrFailure = signalPreparation.Failure(); return;
            }
            const std::array noisy{ frame.rr.packed[0].get(), frame.rr.packed[1].get(),
                frame.outputs.textures[DiffuseAO].get(), frame.outputs.textures[SpecularAO].get() };
            for (uint32_t i = 0; i < 4; ++i) {
                if (!(mask & (1u << i))) continue;
                RedstoneDenoiser::NativeResources resources{};
                for (uint32_t guide = 0; guide < 5; ++guide) resources.guides[guide] = frame.rr.guides[guide].get();
                resources.input = noisy[i]; resources.output = frame.rr.outputs[i].get();
                resources.nativeSize = frame.size; resources.logicalSize = frame.outputs.grid.logical;
                resources.frame = number; resources.origin = frame.outputs.grid.origin; resources.checkerboard = frame.outputs.grid.checkerboard;
                resources.historyRevision = historyRevision;
                const auto otherBytes = ResidentBytes() + reserve + rawReserve;
                resources.contextBudgetBytes = otherBytes < budget ? budget - otherBytes : 0;
                if (nativeDenoiser.RecordNativeSignal(frame.commands.get(), resources, i)) frame.rr.recordedMask |= 1u << i;
            }
            rrFailure.clear();
        } catch (const std::exception& error) { rrFailure = error.what(); }
    }
}

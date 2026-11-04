#include "NativeHybridGI.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Features/LinearLighting.h"
#include "Globals.h"
#include "GpuPass.h"

namespace DX12Native
{
    void NativeHybridGI::RecordRR(Frame& frame, uint32_t slot, uint32_t number, const RedstoneGI& settings)
    {
        CS_GPU_PASS("RedstoneGI::NativeRayRegeneration");
        frame.rr.recordedMask = 0;
        const uint32_t mask = settings.settings.NativeRayRegeneration && RayRegeneration::GetSettings().enabled ? settings.GetNativeSignalMask() : 0u;
        nativeDenoiser.RetireNative(15u & ~mask);
        if (!mask) { frame.rr = {}; return; }
        try {
            if (!nativeDenoiser.InitializeNative(backend)) { rrFailure = "Native RR SDK unavailable; raw GI/AO retained"; return; }
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
                if (nativeDenoiser.RecordNativeSignal(frame.commands.get(), resources, i)) frame.rr.recordedMask |= 1u << i;
            }
            rrFailure.clear();
        } catch (const std::exception& error) { rrFailure = error.what(); }
    }
}

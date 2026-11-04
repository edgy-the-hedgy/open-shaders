#include "DirectionalTrace.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Globals.h"
#include "GpuPass.h"

namespace DX12Native
{
    void DirectionalTrace::RecordNativeComposition(Frame& frame)
    {
        CS_GPU_PASS("RTDirectLighting::NativeComposition");
        frame.composedValid = false;
        if (!frame.publishToScene) return;
        try {
            if (!nativeComposition.Initialize(backend.d3d12Device.get(), "Data/Shaders/RTDirectLighting/nativeDirectCompose.dxil")) {
                nativeCompositionFailure = nativeComposition.Failure(); return;
            }
            for (uint32_t i = 0; i < frame.composed.size(); ++i) {
                if (frame.composed[i]) continue;
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = frame.size[0]; desc.Height = frame.size[1];
                desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                frame.composed[i] = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(),
                    i ? "RTIndirectLighting::NativeComposedLighting" : "RTDirectLighting::NativeComposedLighting");
            }
            SnapshotNativeCamera(backend.d3d12Device.get(), frame.nativeCamera);
            frame.compositionBindings = nativeComposition.RetainBindings();
            const auto& direct = frame.nativeDirectRR;
            const auto& indirect = frame.nativeIndirectRR;
            const std::array<uint32_t, 16> constants{ frame.size[0], frame.size[1], direct.grid.logical[0], direct.grid.logical[1],
                indirect.grid.logical[0], indirect.grid.logical[1], direct.recordedMask, indirect.recordedMask,
                std::bit_cast<uint32_t>(frame.indirectStrength), uint32_t(frame.indirectRays != 0u), frame.directTraceMode };
            const std::array reads{ frame.rawSignals[0].get(), frame.rawSignals[1].get(), frame.rawSignals[3].get(),
                frame.rawSignals[4].get(), frame.rawSignals[5].get(), frame.rawSignals[6].get(),
                direct.outputs[0].get(), direct.outputs[1].get(), indirect.outputs[0].get(), indirect.outputs[1].get(),
                direct.guides[0].get(), indirect.guides[0].get(), frame.receivers->resource.get() };
            const std::array writes{ frame.composed[0]->resource.get(), frame.composed[1]->resource.get() };
            nativeComposition.Dispatch(backend.d3d12Device.get(), frame.commands.get(), uint32_t(&frame - frames.data()), reads, writes,
                constants, frame.nativeCamera->GetGPUVirtualAddress(), frame.size);
            frame.composedValid = true; nativeCompositionFailure.clear();
        } catch (const std::exception& error) { nativeCompositionFailure = error.what(); }
    }
}

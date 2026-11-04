#include "NativeHybridGI.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Features/LinearLighting.h"
#include "Globals.h"
#include "GpuPass.h"

namespace DX12Native
{
    void NativeHybridGI::RecordComposition(Frame& frame, uint32_t slot, const RedstoneGI& settings)
    {
        CS_GPU_PASS("RedstoneGI::NativeComposition");
        frame.composedValid = false;
        try {
            if (!composition.Initialize(backend.d3d12Device.get(), "Data/Shaders/RTDirectLighting/nativeHybridCompose.dxil")) {
                compositionFailure = composition.Failure(); return;
            }
            if (!frame.composed) {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = frame.size[0]; desc.Height = frame.size[1];
                desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                frame.composed = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "RedstoneGI::NativeComposedLighting");
            }
            const auto& grid = frame.outputs.grid;
            const auto strengths = settings.GetNativeContributions();
            const auto occlusion = settings.GetNativeOcclusionSettings();
            const std::array<uint32_t, 16> constants{ frame.size[0], frame.size[1], grid.logical[0], grid.logical[1],
                grid.packed[0], grid.packed[1], uint32_t(grid.checkerboard), grid.origin, frame.rr.recordedMask,
                settings.GetNativeSignalMask(), uint32_t(globals::features::linearLighting.IsLinearLightingActive()),
                std::bit_cast<uint32_t>(strengths.x), std::bit_cast<uint32_t>(strengths.y),
                std::bit_cast<uint32_t>(occlusion.diffuseStrength), std::bit_cast<uint32_t>(occlusion.specularStrength),
                std::bit_cast<uint32_t>(settings.settings.DepthFadeRange.y) };
            const std::array reads{ frame.outputs.textures[Diffuse].get(), frame.outputs.textures[Specular].get(),
                frame.outputs.textures[DiffuseAO].get(), frame.outputs.textures[SpecularAO].get(),
                frame.rr.outputs[0].get(), frame.rr.outputs[1].get(), frame.rr.outputs[2].get(), frame.rr.outputs[3].get(),
                frame.sources[3]->resource.get(), frame.sources[4]->resource.get(), frame.sources[0]->resource.get(),
                frame.sources[1]->resource.get(), frame.outputs.textures[Eligibility].get() };
            const std::array writes{ frame.composed->resource.get() };
            composition.Dispatch(backend.d3d12Device.get(), frame.commands.get(), slot, reads, writes, constants,
                frame.outputs.constants->GetGPUVirtualAddress() + kCameraSnapshotOffset, frame.size);
            frame.composedValid = true; compositionFailure.clear();
        } catch (const std::exception& error) { compositionFailure = error.what(); }
    }
}

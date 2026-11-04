#include "SignalPreparation.h"
#include "GpuPass.h"

namespace DX12Native
{
    bool SignalPreparation::Record(ID3D12Device* device, ID3D12GraphicsCommandList* commands, Frame& frame,
        uint32_t slot, const SignalGrid& grid, std::span<ID3D12Resource* const> inputs,
        bool dxrMaterials, bool direct, bool packedSource, bool linearLighting, float roughnessFloor, uint32_t requestedMask)
    {
        CS_GPU_PASS("RayRegeneration::PrepareNativeSignals");
        frame.recordedMask = 0;
        if (!pipeline.Initialize(device, "Data/Shaders/RTDirectLighting/nativeSignalPreparation.dxil")) return false;
        if (frame.grid.logical != grid.logical || frame.grid.packed != grid.packed) frame = {};
        frame.grid = grid;
        for (uint32_t i = 0; i < frame.guides.size(); ++i)
            if (!frame.guides[i]) frame.guides[i] = CreateNativeTexture(device, grid.logical,
                i ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32_FLOAT, L"RayRegeneration::NativeGuide");
        for (auto& packed : frame.packed)
            if (!packed) packed = CreateNativeTexture(device, grid.packed, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RayRegeneration::RawPackedSignal");
        if (!frame.validity) frame.validity = CreateNativeTexture(device, grid.packed, DXGI_FORMAT_R32_UINT, L"RayRegeneration::IndependentLobeValidity");
        for (uint32_t i = 0; i < 4; ++i) {
            if (!(requestedMask & (1u << i))) { frame.outputs[i] = nullptr; continue; }
            if (!frame.outputs[i]) frame.outputs[i] = CreateNativeTexture(device, grid.logical,
                i < 2 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8_UNORM, L"RayRegeneration::NativeOutput");
        }
        SnapshotNativeCamera(device, frame.camera);
        const std::array<uint32_t, 16> constants{ grid.native[0], grid.native[1], grid.logical[0], grid.logical[1],
            grid.packed[0], grid.packed[1], uint32_t(grid.checkerboard), grid.origin, uint32_t(dxrMaterials),
            uint32_t(direct), uint32_t(packedSource), uint32_t(linearLighting), std::bit_cast<uint32_t>(roughnessFloor), 0u, 0u, 0u };
        const std::array writes{ frame.guides[0].get(), frame.guides[1].get(), frame.guides[2].get(),
            frame.guides[3].get(), frame.guides[4].get(), frame.packed[0].get(), frame.packed[1].get(), frame.validity.get() };
        pipeline.Dispatch(device, commands, slot, inputs, writes, constants, frame.camera->GetGPUVirtualAddress(), grid.logical);
        return true;
    }
}

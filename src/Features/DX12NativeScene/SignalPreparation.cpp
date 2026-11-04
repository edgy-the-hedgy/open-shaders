#include "SignalPreparation.h"
#include "GpuPass.h"
#include "Globals.h"
#include <directx/d3dx12.h>

namespace DX12Native
{
    uint64_t SignalPreparation::Frame::Bytes(ID3D12Device* device) const
    {
        uint64_t result = NativeAllocationBytes(device, validity.get()) + NativeAllocationBytes(device, camera.get());
        for (const auto& value : guides) result += NativeAllocationBytes(device, value.get());
        for (const auto& value : packed) result += NativeAllocationBytes(device, value.get());
        for (const auto& value : outputs) result += NativeAllocationBytes(device, value.get());
        return result;
    }

    uint64_t SignalPreparation::EstimateBytes(ID3D12Device* device, const SignalGrid& grid, uint32_t mask)
    {
        const auto upload = CD3DX12_RESOURCE_DESC::Buffer((sizeof(globals::FrameBuffer) + 255u) & ~255u);
        uint64_t result = device->GetResourceAllocationInfo(0, 1, &upload).SizeInBytes;
        auto add = [&](std::array<uint32_t, 2> size, DXGI_FORMAT format) {
            const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, size[0], size[1], 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            result += device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        };
        for (uint32_t i = 0; i < 5; ++i) add(grid.logical, i ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32_FLOAT);
        for (uint32_t i = 0; i < 3; ++i) add(grid.packed, i < 2 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32_UINT);
        for (uint32_t i = 0; i < 4; ++i) if (mask & (1u << i)) add(grid.logical, i < 2 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8_UNORM);
        return result;
    }
    bool SignalPreparation::Record(ID3D12Device* device, ID3D12GraphicsCommandList* commands, Frame& frame,
        uint32_t slot, const SignalGrid& grid, std::span<ID3D12Resource* const> inputs,
        bool dxrMaterials, bool direct, bool packedSource, bool linearLighting, float roughnessFloor, uint32_t requestedMask)
    {
        CS_GPU_PASS("RayRegeneration::PrepareNativeSignals");
        frame.recordedMask = 0;
        if (!pipeline.Initialize(device, "Data/Shaders/RTDirectLighting/nativeSignalPreparation.dxil")) return false;
        if (frame.grid.logical != grid.logical || frame.grid.packed != grid.packed) frame = {};
        frame.grid = grid;
        frame.bindings = pipeline.RetainBindings();
        const std::wstring producer = direct ? L"RTDirectLighting" : dxrMaterials ? L"RTIndirectLighting" : L"RedstoneGI";
        constexpr std::array guideNames{ L"LinearDepth", L"Motion", L"NormalRoughness", L"DiffuseAlbedo", L"SpecularAlbedo" };
        for (uint32_t i = 0; i < frame.guides.size(); ++i)
            if (!frame.guides[i]) frame.guides[i] = CreateNativeTexture(device, grid.logical,
                i ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32_FLOAT, std::format(L"{}::RR{}", producer, guideNames[i]).c_str());
        for (auto& packed : frame.packed)
            if (!packed) packed = CreateNativeTexture(device, grid.packed, DXGI_FORMAT_R16G16B16A16_FLOAT, L"RayRegeneration::RawPackedSignal");
        if (!frame.validity) frame.validity = CreateNativeTexture(device, grid.packed, DXGI_FORMAT_R32_UINT, L"RayRegeneration::IndependentLobeValidity");
        constexpr std::array outputNames{ L"Diffuse", L"Specular", L"AmbientOcclusion", L"SpecularOcclusion" };
        for (uint32_t i = 0; i < 4; ++i) {
            if (!(requestedMask & (1u << i))) { frame.outputs[i] = nullptr; continue; }
            if (!frame.outputs[i]) frame.outputs[i] = CreateNativeTexture(device, grid.logical,
                i < 2 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8_UNORM, std::format(L"{}::RROutput{}", producer, outputNames[i]).c_str());
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

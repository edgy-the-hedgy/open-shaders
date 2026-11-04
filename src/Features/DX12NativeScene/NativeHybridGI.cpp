#include "NativeHybridGI.h"
#include "Globals.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Features/LinearLighting.h"
#include "Utils/D3D.h"
#include <directx/d3dx12.h>
#include "GpuPass.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace DX12Native
{
    namespace
    {
        constexpr uint32_t kPerPass = 10;
        constexpr uint32_t kPerFrame = kPerPass * 2;
        constexpr uint64_t kUploadBytes = 4096;
        constexpr uint64_t kGISettingsOffset = 0;
        constexpr uint64_t kAOSettingsOffset = 768;
        constexpr uint64_t kLightingOffset = 1024;
        constexpr uint64_t kCameraOffset = NativeHybridGI::kCameraSnapshotOffset;
        constexpr uint64_t kSamplingOffset = 2304;
        struct NativeLightingCB
        {
            float4 camera;
            uint32_t linear, aces, adaptive, padding = 0;
        };
        DXGI_FORMAT OutputFormat(uint32_t index)
        {
            switch (index) {
            case NativeHybridGI::Specular: case NativeHybridGI::Diffuse: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case NativeHybridGI::Eligibility: return DXGI_FORMAT_R32_UINT;
            default: return DXGI_FORMAT_R8_UNORM;
            }
        }
        std::vector<char> ReadDXIL(const char* path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file) throw std::runtime_error(std::string("Native HybridGI DXIL missing: ") + path);
            const auto length = file.tellg();
            if (length <= 0 || length > 16 * 1024 * 1024)
                throw std::runtime_error("Invalid native HybridGI DXIL size");
            std::vector<char> bytes(static_cast<size_t>(length));
            file.seekg(0);
            if (!file.read(bytes.data(), std::streamsize(bytes.size())))
                throw std::runtime_error("Failed to read native HybridGI DXIL");
            return bytes;
        }
    }

    bool NativeHybridGI::ShadersAvailable()
    {
        std::ifstream gi("Data/Shaders/RTDirectLighting/hybridGI.dxil", std::ios::binary);
        std::ifstream ao("Data/Shaders/RTDirectLighting/hybridAO.dxil", std::ios::binary);
        return bool(gi) && bool(ao);
    }

    uint64_t NativeHybridGI::EstimateBytes(ID3D12Device* device, uint32_t width, uint32_t height)
    {
        if (!device || !width || !height) return UINT64_MAX;
        uint64_t total = kUploadBytes;
        for (uint32_t i = 0; i < kOutputs; ++i) {
            const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(OutputFormat(i), width, height, 1, 1, 1, 0,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            const auto bytes = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
            if (bytes == UINT64_MAX || UINT64_MAX - total < bytes) return UINT64_MAX;
            total += bytes;
        }
        return total;
    }

    bool NativeHybridGI::Initialize(ID3D12Device* device)
    {
        if (pipelines[0] && pipelines[1] && descriptors && root) return true;
        try {
            if (!ShadersAvailable()) throw std::runtime_error("Native GI/AO DXIL not deployed; build NativeHybridDXIL");
            auto giCode = ReadDXIL("Data/Shaders/RTDirectLighting/hybridGI.dxil");
            auto aoCode = ReadDXIL("Data/Shaders/RTDirectLighting/hybridAO.dxil");
            D3D12_DESCRIPTOR_RANGE ranges[2]{};
            ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 7, 0, 0, 0 };
            ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0 };
            D3D12_ROOT_PARAMETER params[7]{};
            for (uint32_t i = 0; i < 2; ++i) {
                params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[i].DescriptorTable = { 1, &ranges[i] };
            }
            constexpr std::array<uint32_t, 5> registers{ 1, 2, 5, 12, 13 };
            for (uint32_t i = 0; i < registers.size(); ++i) {
                params[i + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                params[i + 2].Descriptor = { registers[i], 0 };
            }
            D3D12_STATIC_SAMPLER_DESC samplers[2]{};
            for (uint32_t i = 0; i < 2; ++i) {
                samplers[i].Filter = i ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
                samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                samplers[i].ShaderRegister = i;
                samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
                samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            }
            const D3D12_ROOT_SIGNATURE_DESC desc{ 7, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE };
            winrt::com_ptr<ID3DBlob> serialized, errors;
            DX::ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                serialized.put(), errors.put()));
            DX::ThrowIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                serialized->GetBufferSize(), IID_PPV_ARGS(root.put())));
            for (uint32_t i = 0; i < pipelines.size(); ++i) {
                const auto& code = i ? aoCode : giCode;
                D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
                state.pRootSignature = root.get();
                state.CS = { code.data(), code.size() };
                DX::ThrowIfFailed(device->CreateComputePipelineState(&state, IID_PPV_ARGS(pipelines[i].put())));
            }
            const D3D12_DESCRIPTOR_HEAP_DESC heap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                kPerFrame * 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
            DX::ThrowIfFailed(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(descriptors.put())));
            failure.clear();
            return true;
        } catch (const std::exception& e) {
            for (auto& pipeline : pipelines) pipeline = nullptr;
            descriptors = nullptr;
            root = nullptr;
            failure = e.what();
            return false;
        }
    }

    bool NativeHybridGI::Record(ID3D12Device* device, ID3D12GraphicsCommandList* list,
        FrameOutputs& frame, uint32_t slot, uint32_t frameNumber,
        const std::array<uint32_t, 2>& size, ID3D12Resource* linearDepth,
        ID3D12Resource* normalRough, ID3D12Resource* linearRadiance,
        ID3D12Resource* albedo, ID3D12Resource* reflectance,
        const RedstoneGI& settings)
    {
        CS_GPU_PASS("RedstoneGI::NativeEstimators");
        frame.valid = false;
        if (slot >= 2 || !list || !linearDepth || !normalRough || !linearRadiance ||
            !albedo || !reflectance || !Initialize(device)) return false;
        try {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            for (uint32_t i = 0; i < kOutputs; ++i) {
                if (frame.textures[i]) continue;
                auto desc = CD3DX12_RESOURCE_DESC::Tex2D(OutputFormat(i), frame.grid.packed[0], frame.grid.packed[1], 1, 1, 1, 0,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                DX::ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(frame.textures[i].put())));
            }
            if (!frame.constants) {
                const auto upload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
                const auto desc = CD3DX12_RESOURCE_DESC::Buffer(kUploadBytes);
                DX::ThrowIfFailed(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(frame.constants.put())));
            }
            auto nativeGI = settings.GetNativeGISettings(size, frameNumber);
            nativeGI.RRCheckerboardEnabled = frame.grid.checkerboard;
            nativeGI.RRCheckerboardOrigin = frame.grid.origin;
            nativeGI.MinScreenRadius = settings.settings.MinScreenRadius * float(frame.grid.logical[0]);
            auto nativeAO = settings.GetNativeOcclusionSettings();
            const NativeLightingCB light{ Util::GetCameraData(),
                uint32_t(globals::features::linearLighting.IsLinearLightingActive()),
                globals::features::linearLighting.settings.enableACEScg, uint32_t(settings.settings.EnableAdaptiveSampling) };
            const auto& camera = globals::game::frameBufferCached.nonVR;
            if (sizeof(nativeGI) > kAOSettingsOffset ||
                sizeof(nativeAO) > kLightingOffset - kAOSettingsOffset ||
                sizeof(light) > kCameraOffset - kLightingOffset ||
                sizeof(camera) > kSamplingOffset - kCameraOffset)
                throw std::runtime_error("Native HybridGI constant-buffer layout exceeded");
            uint8_t* mapped = nullptr;
            const D3D12_RANGE noRead{ 0, 0 };
            DX::ThrowIfFailed(frame.constants->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
            std::memcpy(mapped + kGISettingsOffset, &nativeGI, sizeof(nativeGI));
            std::memcpy(mapped + kAOSettingsOffset, &nativeAO, sizeof(nativeAO));
            std::memcpy(mapped + kLightingOffset, &light, sizeof(light));
            std::memcpy(mapped + kCameraOffset, &camera, sizeof(camera));
            struct Sampling { std::array<uint32_t, 2> logical, packed; uint32_t checkerboard, origin, clamp; float threshold; };
            const Sampling sampling{ frame.grid.logical, frame.grid.packed, uint32_t(frame.grid.checkerboard),
                frame.grid.origin, uint32_t(settings.settings.NativeRadianceClamp), settings.settings.NativeRadianceThreshold };
            std::memcpy(mapped + kSamplingOffset, &sampling, sizeof(sampling));
            frame.constants->Unmap(0, nullptr);

            const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            const auto sources = std::array<ID3D12Resource*, 7>{ linearDepth, normalRough,
                linearRadiance, nullptr, nullptr, albedo, reflectance };
            const auto formats = std::array<DXGI_FORMAT, 7>{
                DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT };
            const std::array<uint32_t, 3> giUAV{ 0u, 1u, 2u };
            const std::array<uint32_t, 3> aoUAV{ 3u, 4u, 2u };
            for (uint32_t pass = 0; pass < 2; ++pass) {
                auto cpu = descriptors->GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += uint64_t(slot * kPerFrame + pass * kPerPass) * step;
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                for (uint32_t i = 0; i < sources.size(); ++i) {
                    srv.Format = formats[i];
                    device->CreateShaderResourceView(sources[i], &srv, cpu);
                    cpu.ptr += step;
                }
                D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                for (uint32_t i = 0; i < 3; ++i) {
                    const auto index = pass ? aoUAV[i] : giUAV[i];
                    uav.Format = OutputFormat(index);
                    device->CreateUnorderedAccessView(pass && i == 2 ? nullptr : frame.textures[index].get(), nullptr, &uav, cpu);
                    cpu.ptr += step;
                }
            }
            std::vector<D3D12_RESOURCE_BARRIER> begin;
            for (auto* source : sources)
                if (source) begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(source,
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
            for (const auto& target : frame.textures)
                begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(target.get(),
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
            list->ResourceBarrier(UINT(begin.size()), begin.data());

            ID3D12DescriptorHeap* heaps[]{ descriptors.get() };
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(root.get());
            const auto base = frame.constants->GetGPUVirtualAddress();
            list->SetComputeRootConstantBufferView(2, base + kGISettingsOffset);
            list->SetComputeRootConstantBufferView(3, base + kAOSettingsOffset);
            list->SetComputeRootConstantBufferView(4, base + kLightingOffset);
            list->SetComputeRootConstantBufferView(5, base + kCameraOffset);
            list->SetComputeRootConstantBufferView(6, base + kSamplingOffset);
            for (uint32_t pass = 0; pass < 2; ++pass) {
                list->SetPipelineState(pipelines[pass].get());
                auto gpu = descriptors->GetGPUDescriptorHandleForHeapStart();
                gpu.ptr += uint64_t(slot * kPerFrame + pass * kPerPass) * step;
                list->SetComputeRootDescriptorTable(0, gpu);
                gpu.ptr += 7 * step;
                list->SetComputeRootDescriptorTable(1, gpu);
                list->Dispatch((frame.grid.packed[0] + 7u) / 8u, (frame.grid.packed[1] + 7u) / 8u, 1);
                if (pass == 0) ++giDispatches;
                else ++aoDispatches;
            }
            std::vector<D3D12_RESOURCE_BARRIER> end;
            for (auto* source : sources)
                if (source) end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(source,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
            for (const auto& target : frame.textures)
                end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(target.get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
            list->ResourceBarrier(UINT(end.size()), end.data());
            frame.valid = true;
            failure.clear();
            return true;
        } catch (const std::exception& error) {
            frame.valid = false;
            failure = error.what();
            return false;
        }
    }
}

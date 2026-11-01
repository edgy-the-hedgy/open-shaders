#include "NativeHybridGI.h"
#include "Globals.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Features/LinearLighting.h"
#include "Utils/D3D.h"
#include <directx/d3dx12.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <fstream>
#include <vector>

namespace DX12Native
{
    namespace
    {
        constexpr uint32_t kPerPass = 11; // t0..t7 then u0..u2
        constexpr uint32_t kPerFrame = kPerPass * 2;
        constexpr uint64_t kUploadBytes = 4096;
        constexpr uint64_t kGISettingsOffset = 0;
        constexpr uint64_t kAOSettingsOffset = 768;
        constexpr uint64_t kLightingOffset = 1024;
        constexpr uint64_t kCameraOffset = 1280;
        constexpr uint64_t kMaskOffset = 2048;
        constexpr uint64_t kCompositionOffset = 2304;
        constexpr uint32_t kCompositionDescriptorsPerFrame = 11;
        struct NativeLightingCB
        {
            float4 camera;
            uint32_t linear, aces, padding[2]{};
        };
        struct NativeMaskCB
        {
            float influence, threshold;
            float minimumValidDepth, maximumValidDepth;
        };
        struct NativeCompositionCB
        {
            uint32_t width, height;
            float directBlend, giContribution;
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
        std::ifstream eligibility("Data/Shaders/RTDirectLighting/hybridEligibility.dxil", std::ios::binary);
        return bool(gi) && bool(ao) && bool(eligibility);
    }

    uint64_t NativeHybridGI::EstimateBytes(ID3D12Device* device, uint32_t width, uint32_t height,
        const HybridSamplingLayout& sampling)
    {
        if (!device || !width || !height) return UINT64_MAX;
        uint64_t total = kUploadBytes;
        for (uint32_t i = 0; i < kOutputs; ++i) {
            const auto extent = i == Eligibility ? std::array<uint32_t, 2>{width, height} : sampling.packed;
            if (!extent[0] || !extent[1]) return UINT64_MAX;
            const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(OutputFormat(i), extent[0], extent[1], 1, 1, 1, 0,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            const auto bytes = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
            if (bytes == UINT64_MAX || UINT64_MAX - total < bytes) return UINT64_MAX;
            total += bytes;
        }
        // The final scene is also allocated on DX12, then exported once.
        const auto finalDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, 1, 1);
        const auto finalBytes = device->GetResourceAllocationInfo(0, 1, &finalDesc).SizeInBytes;
        if (finalBytes == UINT64_MAX || UINT64_MAX - total < finalBytes) return UINT64_MAX;
        return total + finalBytes;
    }

    bool NativeHybridGI::Initialize(ID3D12Device* device)
    {
        if (pipelines[0] && pipelines[1] && eligibilityPipeline && descriptors && root) return true;
        try {
            if (!ShadersAvailable()) throw std::runtime_error("Native GI/AO DXIL not deployed; build RTComposerHybridDXIL");
            auto giCode = ReadDXIL("Data/Shaders/RTDirectLighting/hybridGI.dxil");
            auto aoCode = ReadDXIL("Data/Shaders/RTDirectLighting/hybridAO.dxil");
            auto eligibilityCode = ReadDXIL("Data/Shaders/RTDirectLighting/hybridEligibility.dxil");
            D3D12_DESCRIPTOR_RANGE ranges[2]{};
            ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0 };
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
            D3D12_COMPUTE_PIPELINE_STATE_DESC eligibilityState{};
            eligibilityState.pRootSignature = root.get();
            eligibilityState.CS = { eligibilityCode.data(), eligibilityCode.size() };
            DX::ThrowIfFailed(device->CreateComputePipelineState(
                &eligibilityState, IID_PPV_ARGS(eligibilityPipeline.put())));
            const D3D12_DESCRIPTOR_HEAP_DESC heap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                kPerFrame * 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
            DX::ThrowIfFailed(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(descriptors.put())));
            failure.clear();
            return true;
        } catch (const std::exception& e) {
            // A partially created root/PSO/descriptor heap cannot be reused.
            // Reinitialize cleanly after a shader deployment or cache reload.
            for (auto& pipeline : pipelines) pipeline = nullptr;
            eligibilityPipeline = nullptr;
            descriptors = nullptr;
            root = nullptr;
            failure = e.what();
            return false;
        }
    }

    bool NativeHybridGI::Record(ID3D12Device* device, ID3D12GraphicsCommandList* list,
        FrameOutputs& frame, uint32_t slot, uint32_t frameNumber,
        const std::array<uint32_t, 2>& size,
        const HybridSamplingLayout& sampling, ID3D12Resource* linearDepth,
        ID3D12Resource* normalRough, ID3D12Resource* linearRadiance,
        ID3D12Resource* albedo, ID3D12Resource* reflectance, ID3D12Resource* shadowMask,
        const RedstoneGI& settings, float shadowInfluence, float skipThreshold)
    {
        frame.valid = false;
        if (slot >= 2 || !list || !linearDepth || !normalRough || !linearRadiance ||
            !albedo || !reflectance || !shadowMask || !sampling.packed[0] ||
            !sampling.packed[1] || !Initialize(device)) return false;
        try {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            // Slots are reused only after their completion fence retires.
            // Recreate raw signal textures when the logical RR grid changes;
            // the full-native eligibility texture retains its own dimensions.
            if (frame.sampling.native != sampling.native) {
                for (auto& texture : frame.textures) texture = nullptr;
                frame.composedScene.reset();
            } else if (frame.sampling.packed != sampling.packed) {
                for (uint32_t i = 0; i < kOutputs; ++i)
                    if (i != Eligibility) frame.textures[i] = nullptr;
            }
            frame.sampling = sampling;
            for (uint32_t i = 0; i < kOutputs; ++i) {
                if (frame.textures[i]) continue;
                const auto extent = i == Eligibility ? size : sampling.packed;
                auto desc = CD3DX12_RESOURCE_DESC::Tex2D(OutputFormat(i), extent[0], extent[1], 1, 1, 1, 0,
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
            auto nativeGI = settings.GetNativeGISettings(size, frameNumber, sampling);
            auto nativeAO = settings.GetNativeOcclusionSettings();
            const NativeLightingCB light{ Util::GetCameraData(),
                uint32_t(globals::features::linearLighting.IsLinearLightingActive()),
                globals::features::linearLighting.settings.enableACEScg };
            const NativeMaskCB mask{ shadowInfluence, skipThreshold, 18.0f,
                nativeGI.DepthFadeRange.y };
            const auto& camera = globals::game::frameBufferCached.nonVR;
            if (sizeof(nativeGI) > kAOSettingsOffset ||
                sizeof(nativeAO) > kLightingOffset - kAOSettingsOffset ||
                sizeof(light) > kCameraOffset - kLightingOffset ||
                sizeof(camera) > kMaskOffset - kCameraOffset ||
                sizeof(mask) > kUploadBytes - kMaskOffset)
                throw std::runtime_error("Native HybridGI constant-buffer layout exceeded");
            uint8_t* mapped = nullptr;
            const D3D12_RANGE noRead{ 0, 0 };
            DX::ThrowIfFailed(frame.constants->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
            std::memcpy(mapped + kGISettingsOffset, &nativeGI, sizeof(nativeGI));
            std::memcpy(mapped + kAOSettingsOffset, &nativeAO, sizeof(nativeAO));
            std::memcpy(mapped + kLightingOffset, &light, sizeof(light));
            std::memcpy(mapped + kCameraOffset, &camera, sizeof(camera));
            std::memcpy(mapped + kMaskOffset, &mask, sizeof(mask));
            frame.constants->Unmap(0, nullptr);

            const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            const auto sources = std::array<ID3D12Resource*, 8>{ linearDepth, normalRough,
                linearRadiance, nullptr, nullptr, albedo, reflectance, shadowMask };
            const auto formats = std::array<DXGI_FORMAT, 8>{
                DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT };
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
                    device->CreateUnorderedAccessView(frame.textures[index].get(), nullptr, &uav, cpu);
                    cpu.ptr += step;
                }
            }
            // Complete resource creation and descriptor publication before
            // recording the first barrier; failures cannot leave a half-pass.
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
            list->SetComputeRootConstantBufferView(6, base + kMaskOffset);
            for (uint32_t pass = 0; pass < 2; ++pass) {
                list->SetPipelineState(pipelines[pass].get());
                auto gpu = descriptors->GetGPUDescriptorHandleForHeapStart();
                gpu.ptr += uint64_t(slot * kPerFrame + pass * kPerPass) * step;
                list->SetComputeRootDescriptorTable(0, gpu);
                gpu.ptr += 8 * step;
                list->SetComputeRootDescriptorTable(1, gpu);
                list->Dispatch((sampling.packed[0] + 7u) / 8u,
                    (sampling.packed[1] + 7u) / 8u, 1);
                if (pass == 0) ++giDispatches;
                else ++aoDispatches;
            }
            // Full-resolution eligibility is never estimated from quarter- or
            // checkerboard-traced outputs. It uses native depth, normal and
            // shadow visibility and remains authoritative after RR/upscaling.
            list->SetPipelineState(eligibilityPipeline.get());
            auto eligibilityGPU = descriptors->GetGPUDescriptorHandleForHeapStart();
            eligibilityGPU.ptr += uint64_t(slot * kPerFrame) * step;
            list->SetComputeRootDescriptorTable(0, eligibilityGPU);
            eligibilityGPU.ptr += 8u * step;
            list->SetComputeRootDescriptorTable(1, eligibilityGPU);
            list->Dispatch((size[0] + 7u) / 8u, (size[1] + 7u) / 8u, 1);
            ++eligibilityDispatches;
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

    bool NativeHybridGI::RecordComposition(ID3D12Device* device, ID3D11Device5* device11,
        ID3D12GraphicsCommandList* list, FrameOutputs& frame, uint32_t slot,
        const std::array<uint32_t, 2>& size, ID3D12Resource* shadowedSource,
        ID3D12Resource* nativeDirect, float directBlend, float giContribution)
    {
        frame.composedValid = false;
        if (!frame.valid || !device || !device11 || !list || !shadowedSource ||
            !nativeDirect || slot >= 2 || !size[0] || !size[1]) {
            compositionFailure = "Missing valid GI/AO, source, or DX12 composition resources";
            return false;
        }
        try {
            if (!root) throw std::runtime_error("Native GI root signature unavailable");
            if (!compositionPipeline) {
                auto bytecode = ReadDXIL("Data/Shaders/RTDirectLighting/nativeHybridCompose.dxil");
                D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
                state.pRootSignature = root.get();
                state.CS = { bytecode.data(), bytecode.size() };
                DX::ThrowIfFailed(device->CreateComputePipelineState(
                    &state, IID_PPV_ARGS(compositionPipeline.put())));
            }
            if (!compositionDescriptors) {
                const D3D12_DESCRIPTOR_HEAP_DESC heap{
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                    kCompositionDescriptorsPerFrame * 2,
                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0
                };
                DX::ThrowIfFailed(device->CreateDescriptorHeap(
                    &heap, IID_PPV_ARGS(compositionDescriptors.put())));
            }
            if (!frame.composedScene) {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = size[0];
                desc.Height = size[1];
                desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                frame.composedScene = std::make_unique<WrappedResource>(
                    desc, device11, device, "RTComposer::NativeHybridComposedScene");
            }

            const NativeCompositionCB constants{ size[0], size[1],
                std::isfinite(directBlend) ? std::clamp(directBlend, 0.0f, 1.0f) : 0.0f,
                std::isfinite(giContribution) ? std::clamp(giContribution, 0.0f, 1.0f) : 0.0f };
            static_assert(sizeof(constants) == 16);
            static_assert(kCompositionOffset + sizeof(constants) <= kUploadBytes);
            uint8_t* mapped = nullptr;
            const D3D12_RANGE noRead{ 0, 0 };
            DX::ThrowIfFailed(frame.constants->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
            std::memcpy(mapped + kCompositionOffset, &constants, sizeof(constants));
            frame.constants->Unmap(0, nullptr);

            const std::array<ID3D12Resource*, 7> reads{
                shadowedSource, nativeDirect, frame.textures[Diffuse].get(),
                frame.textures[Specular].get(), frame.textures[DiffuseAO].get(),
                frame.textures[SpecularAO].get(), frame.textures[Eligibility].get()
            };
            const std::array<DXGI_FORMAT, 8> formats{
                DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM,
                DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT
            };
            const UINT step = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto cpu = compositionDescriptors->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += uint64_t(slot * kCompositionDescriptorsPerFrame) * step;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            for (uint32_t i = 0; i < formats.size(); ++i) {
                srv.Format = formats[i];
                device->CreateShaderResourceView(i < reads.size() ? reads[i] : nullptr,
                    &srv, cpu);
                cpu.ptr += step;
            }
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            for (uint32_t i = 0; i < 3; ++i) {
                device->CreateUnorderedAccessView(
                    i == 0 ? frame.composedScene->resource.get() : nullptr,
                    nullptr, &uav, cpu);
                cpu.ptr += step;
            }

            std::array<D3D12_RESOURCE_BARRIER, 8> begin{};
            std::array<D3D12_RESOURCE_BARRIER, 8> end{};
            for (uint32_t i = 0; i < reads.size(); ++i) {
                begin[i] = CD3DX12_RESOURCE_BARRIER::Transition(reads[i],
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                end[i] = CD3DX12_RESOURCE_BARRIER::Transition(reads[i],
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COMMON);
            }
            begin[7] = CD3DX12_RESOURCE_BARRIER::Transition(
                frame.composedScene->resource.get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            end[7] = CD3DX12_RESOURCE_BARRIER::Transition(
                frame.composedScene->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON);
            list->ResourceBarrier(UINT(begin.size()), begin.data());
            ID3D12DescriptorHeap* heaps[]{ compositionDescriptors.get() };
            list->SetDescriptorHeaps(1, heaps);
            list->SetPipelineState(compositionPipeline.get());
            list->SetComputeRootSignature(root.get());
            auto gpu = compositionDescriptors->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += uint64_t(slot * kCompositionDescriptorsPerFrame) * step;
            list->SetComputeRootDescriptorTable(0, gpu);
            gpu.ptr += 8 * step;
            list->SetComputeRootDescriptorTable(1, gpu);
            list->SetComputeRootConstantBufferView(6,
                frame.constants->GetGPUVirtualAddress() + kCompositionOffset);
            list->Dispatch((size[0] + 7u) / 8u, (size[1] + 7u) / 8u, 1);
            list->ResourceBarrier(UINT(end.size()), end.data());
            ++compositionDispatches;
            frame.composedValid = true;
            compositionFailure.clear();
            return true;
        } catch (const std::exception& error) {
            compositionFailure = error.what();
            frame.composedValid = false;
            return false;
        }
    }
}

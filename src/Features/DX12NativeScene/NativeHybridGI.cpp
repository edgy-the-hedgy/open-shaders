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
        constexpr uint32_t kPerPass = 12; // t0..t8 then u0..u2
        constexpr uint32_t kPerFrame = kPerPass * 2;
        constexpr uint64_t kUploadBytes = 8192;
        constexpr uint64_t kGISettingsOffset = 0;
        constexpr uint64_t kAOSettingsOffset = 768;
        constexpr uint64_t kLightingOffset = 1024;
        constexpr uint64_t kCameraOffset = 1280;
        constexpr uint64_t kMaskOffset = 2048;
        constexpr uint64_t kCompositionOffset = 2304;
        constexpr uint64_t kRRGuideOffset = 2560;
        constexpr uint64_t kPostOffset = 2816;
        constexpr uint64_t kNativeReconstructionOffset = 5120;
        constexpr uint64_t kNativeTemporalOffset = 6144;
        constexpr uint32_t kNativeTemporalDescriptorsPerSignal = 5u;
        constexpr uint32_t kNativeTemporalDescriptorsPerFrame = 4u * kNativeTemporalDescriptorsPerSignal;
        constexpr uint32_t kNativeDescriptorsPerSignal = 5u;
        constexpr uint32_t kNativeDescriptorsPerFrame = 4u * kNativeDescriptorsPerSignal;
        constexpr uint32_t kPostPerPass = 5;
        constexpr uint32_t kPostPerFrame = kPostPerPass * 4 * 2;
        constexpr uint32_t kRRGuideDescriptorsPerFrame = 10;
        constexpr uint32_t kCompositionDescriptorsPerFrame = 14; // 11 SRVs (mask + depth) + 3 UAVs
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
            uint32_t enableLinearLighting, enableACEScg;
            uint32_t shadowEdgeFilterEnabled;
            float shadowInfluence;
            uint32_t advancedTransportTier;
            uint32_t nativeValidityMask;
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

    bool NativeHybridGI::AdvancedShadersAvailable()
    {
        for (const char* path : {
            "Data/Shaders/RTDirectLighting/nativeAdvancedPack.dxil",
            "Data/Shaders/RTDirectLighting/nativeHybridRRGuides.dxil",
            "Data/Shaders/RTDirectLighting/nativeHybridRRHistory.dxil",
            "Data/Shaders/RTDirectLighting/hybridPostRRReconstruct.dxil",
            "Data/Shaders/RTDirectLighting/hybridPostRRBlur.dxil",
            "Data/Shaders/RTDirectLighting/hybridNativeReconstruct.dxil",
            "Data/Shaders/RTDirectLighting/hybridNativeTemporal.dxil",
            "Data/Shaders/RTDirectLighting/nativeHybridCompose.dxil" }) {
            std::ifstream file(path, std::ios::binary);
            if (!file) return false;
        }
        return true;
    }

    uint64_t NativeHybridGI::EstimateBytes(ID3D12Device* device, uint32_t width, uint32_t height,
        const HybridSamplingLayout& sampling,
        const std::array<bool, 3>& postBlurEnabled)
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
        // RR guides are full logical-grid textures, even for checkerboard
        // signals. Only noisy signal storage is horizontally packed.
        for (uint32_t i = 0; i < FrameOutputs::kGuideCount; ++i) {
            const DXGI_FORMAT format = i == 0 ?
                DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
            const auto guideDesc = CD3DX12_RESOURCE_DESC::Tex2D(format,
                sampling.logical[0], sampling.logical[1], 1, 1, 1, 0,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            const uint64_t guideBytes =
                device->GetResourceAllocationInfo(0, 1, &guideDesc).SizeInBytes;
            if (guideBytes == UINT64_MAX || UINT64_MAX - total < guideBytes)
                return UINT64_MAX;
            total += guideBytes;
        }
        // Logical RR outputs plus native filtered and blur scratch textures.
        for (uint32_t i = 0; i < 4; ++i) {
            const DXGI_FORMAT format = i < 2 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8_UNORM;
            for (uint32_t kind = 0; kind < 3; ++kind) {
                if (kind == 2u && !postBlurEnabled[std::min(i, 2u)]) continue;
                const uint32_t w = kind == 0 ? sampling.logical[0] : width;
                const uint32_t h = kind == 0 ? sampling.logical[1] : height;
                const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, w, h, 1, 1, 1, 0,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                const auto bytes = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
                if (bytes == UINT64_MAX || UINT64_MAX - total < bytes) return UINT64_MAX;
                total += bytes;
            }
        }
        // Two persistent logical history banks are needed for temporal
        // fallback, independent of the AMD SDK output buffers. Count both
        // banks conservatively even when a signal currently opts into RR.
        for (uint32_t bank = 0; bank < 2u; ++bank) {
            for (uint32_t signal = 0; signal < FrameOutputs::kSignals; ++signal) {
                const DXGI_FORMAT format = signal < 2u ?
                    DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8_UNORM;
                const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format,
                    sampling.logical[0], sampling.logical[1], 1, 1, 1, 0,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                const auto bytes = device->GetResourceAllocationInfo(
                    0, 1, &desc).SizeInBytes;
                if (bytes == UINT64_MAX || UINT64_MAX - total < bytes)
                    return UINT64_MAX;
                total += bytes;
            }
        }
        // Persistent logical depth and normal snapshots survive recycled
        // frame slots and are updated with ordered GPU-only copies.
        for (uint32_t i = 0; i < 2; ++i) {
            const auto fmt = i == 0 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
            const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(fmt,
                sampling.logical[0], sampling.logical[1], 1, 1, 1, 0,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            const auto bytes = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
            if (bytes == UINT64_MAX || UINT64_MAX - total < bytes) return UINT64_MAX;
            total += bytes;
        }
        // A logical-grid motion scratch isolates per-pixel history rejections
        // from the original physical motion input.
        const auto motionDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            sampling.logical[0], sampling.logical[1], 1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto motionBytes = device->GetResourceAllocationInfo(
            0, 1, &motionDesc).SizeInBytes;
        if (motionBytes == UINT64_MAX || UINT64_MAX - total < motionBytes)
            return UINT64_MAX;
        total += motionBytes;
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
            ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 9, 0, 0, 0 };
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
        frame.advancedTransportTier = 0u;
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
            const auto sources = std::array<ID3D12Resource*, 9>{ linearDepth, normalRough,
                linearRadiance, nullptr, nullptr, albedo, reflectance, shadowMask, nullptr };
            const auto formats = std::array<DXGI_FORMAT, 9>{
                DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT };
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
                gpu.ptr += 9 * step;
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
            eligibilityGPU.ptr += 9u * step;
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


    bool NativeHybridGI::RecordAdvancedTransport(ID3D12Device* device,
        ID3D12GraphicsCommandList* list, FrameOutputs& frame,
        uint32_t slot, uint32_t frameNumber,
        std::array<uint32_t, 2> nativeSize, uint32_t tier,
        ID3D12Resource* rawDiffuse, ID3D12Resource* rawSpecular,
        ID3D12Resource* receivers, ID3D12Resource* linearDepth)
    {
        frame.valid = frame.rrValid = frame.postValid = frame.composedValid = false;
        frame.rrGuidesValid = frame.rrHistoryValid = false;
        frame.advancedTransportTier = 0u;
        if (!device || !list || slot >= 2u || tier < 2u || tier > 5u ||
            !rawDiffuse || !rawSpecular || !receivers || !linearDepth ||
            !nativeSize[0] || !nativeSize[1]) {
            advancedFailure = "Missing advanced transport resources";
            return false;
        }
        // Checkerboard RR requires an even horizontal logical extent.
        // Never silently treat a half-traced frame as full-native on odd sizes.
        const auto layout = MakeHybridSamplingLayout(
            tier < 4u ? 1u : 0u, tier >= 4u, nativeSize, frameNumber);
        if (tier >= 4u && !layout.checkerboard) {
            advancedFailure = "Advanced native checkerboard requires even horizontal extent";
            return false;
        }
        try {
            if (!advancedPackPipeline || !advancedPackRoot || !advancedPackDescriptors) {
                advancedPackPipeline = nullptr;
                advancedPackRoot = nullptr;
                advancedPackDescriptors = nullptr;
                auto code = ReadDXIL("Data/Shaders/RTDirectLighting/nativeAdvancedPack.dxil");
                D3D12_DESCRIPTOR_RANGE ranges[2]{};
                ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0 };
                ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 5, 0, 0, 0 };
                D3D12_ROOT_PARAMETER params[3]{};
                for (uint32_t i = 0; i < 2u; ++i) {
                    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                    params[i].DescriptorTable = { 1u, &ranges[i] };
                }
                params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                params[2].Constants = { 0u, 0u, 8u };
                const D3D12_ROOT_SIGNATURE_DESC rootDesc{ 3u, params, 0u,
                    nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
                winrt::com_ptr<ID3DBlob> serialized, errors;
                DX::ThrowIfFailed(D3D12SerializeRootSignature(&rootDesc,
                    D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
                DX::ThrowIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                    serialized->GetBufferSize(), IID_PPV_ARGS(advancedPackRoot.put())));
                D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
                state.pRootSignature = advancedPackRoot.get();
                state.CS = { code.data(), code.size() };
                DX::ThrowIfFailed(device->CreateComputePipelineState(
                    &state, IID_PPV_ARGS(advancedPackPipeline.put())));
                const D3D12_DESCRIPTOR_HEAP_DESC desc{
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 9u * 2u,
                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0u };
                DX::ThrowIfFailed(device->CreateDescriptorHeap(
                    &desc, IID_PPV_ARGS(advancedPackDescriptors.put())));
            }
            frame.sampling = layout;
            frame.advancedTransportTier = tier;
            if (!frame.constants) {
                const auto desc = CD3DX12_RESOURCE_DESC::Buffer(kUploadBytes);
                const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
                DX::ThrowIfFailed(device->CreateCommittedResource(&heap,
                    D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr, IID_PPV_ARGS(frame.constants.put())));
            }
            const auto& camera = globals::game::frameBufferCached.nonVR;
            if (sizeof(camera) > kMaskOffset - kCameraOffset)
                throw std::runtime_error("Advanced transport camera constants exceeded");
            uint8_t* mapped = nullptr;
            const D3D12_RANGE noRead{ 0, 0 };
            DX::ThrowIfFailed(frame.constants->Map(0, &noRead,
                reinterpret_cast<void**>(&mapped)));
            std::memcpy(mapped + kCameraOffset, &camera, sizeof(camera));
            frame.constants->Unmap(0, nullptr);

            for (uint32_t i = 0; i < kOutputs; ++i) {
                const auto extent = i == Eligibility ? nativeSize : layout.packed;
                const auto format = OutputFormat(i);
                if (frame.textures[i]) {
                    const auto desc = frame.textures[i]->GetDesc();
                    if (desc.Format == format && desc.Width == extent[0] &&
                        desc.Height == extent[1]) continue;
                    frame.textures[i] = nullptr;
                }
                const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format,
                    extent[0], extent[1], 1, 1, 1, 0,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
                DX::ThrowIfFailed(device->CreateCommittedResource(&heap,
                    D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                    nullptr, IID_PPV_ARGS(frame.textures[i].put())));
            }
            const std::array<ID3D12Resource*, 4> reads{
                rawDiffuse, rawSpecular, receivers, linearDepth };
            const std::array<DXGI_FORMAT, 4> formats{
                DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32_FLOAT };
            const std::array<uint32_t, 5> outputs{
                Diffuse, Specular, DiffuseAO, SpecularAO, Eligibility };
            const UINT step = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto cpu = advancedPackDescriptors->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += uint64_t(slot * 9u) * step;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            for (uint32_t i = 0; i < reads.size(); ++i) {
                srv.Format = formats[i];
                device->CreateShaderResourceView(reads[i], &srv, cpu);
                cpu.ptr += step;
            }
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            for (uint32_t i = 0; i < outputs.size(); ++i) {
                uav.Format = OutputFormat(outputs[i]);
                device->CreateUnorderedAccessView(frame.textures[outputs[i]].get(),
                    nullptr, &uav, cpu);
                cpu.ptr += step;
            }
            std::vector<D3D12_RESOURCE_BARRIER> begin, end;
            for (auto* read : reads) {
                begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(read,
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
                end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(read,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
            }
            for (const auto& output : frame.textures) {
                begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(output.get(),
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
                end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(output.get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
            }
            list->ResourceBarrier(UINT(begin.size()), begin.data());
            ID3D12DescriptorHeap* heaps[]{ advancedPackDescriptors.get() };
            list->SetDescriptorHeaps(1, heaps);
            list->SetPipelineState(advancedPackPipeline.get());
            list->SetComputeRootSignature(advancedPackRoot.get());
            auto gpu = advancedPackDescriptors->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += uint64_t(slot * 9u) * step;
            list->SetComputeRootDescriptorTable(0, gpu);
            gpu.ptr += 4u * step;
            list->SetComputeRootDescriptorTable(1, gpu);
            const uint32_t constants[8]{
                nativeSize[0], nativeSize[1], layout.logical[0], layout.logical[1],
                tier, frameNumber, layout.packed[0], layout.packed[1] };
            list->SetComputeRoot32BitConstants(2, 8u, constants, 0u);
            list->Dispatch((nativeSize[0] + 7u) / 8u,
                (nativeSize[1] + 7u) / 8u, 1u);
            list->ResourceBarrier(UINT(end.size()), end.data());
            ++advancedPackDispatches;
            frame.valid = true;
            advancedFailure.clear();
            return true;
        } catch (const std::exception& error) {
            advancedFailure = error.what();
            frame.valid = false;
            return false;
        }
    }

    bool NativeHybridGI::RecordRRGuides(ID3D12Device* device,
        ID3D12GraphicsCommandList* list, FrameOutputs& frame, uint32_t slot,
        ID3D12Resource* linearDepth, ID3D12Resource* normalRough,
        ID3D12Resource* motion, ID3D12Resource* diffuseAlbedo,
        ID3D12Resource* specularAlbedo)
    {
        frame.rrGuidesValid = false;
        if (!frame.valid || !frame.constants || !device || !list || slot >= 2u ||
            !linearDepth || !normalRough || !motion || !diffuseAlbedo || !specularAlbedo ||
            !frame.sampling.logical[0] || !frame.sampling.logical[1]) {
            guideFailure = "Native RR guide inputs or GI frame unavailable";
            return false;
        }

        try {
            if (!guidePipeline || !guideRoot || !guideDescriptors) {
                // Retry partial initialization safely after deployment/cache reset.
                guidePipeline = nullptr;
                guideRoot = nullptr;
                guideDescriptors = nullptr;
                const auto code = ReadDXIL(
                    "Data/Shaders/RTDirectLighting/nativeHybridRRGuides.dxil");
                D3D12_DESCRIPTOR_RANGE ranges[2]{};
                ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0 };
                ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 5, 0, 0, 0 };
                D3D12_ROOT_PARAMETER params[4]{};
                for (uint32_t i = 0; i < 2; ++i) {
                    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                    params[i].DescriptorTable = { 1, &ranges[i] };
                }
                params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                params[2].Descriptor = { 0, 0 };
                params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                params[3].Descriptor = { 12, 0 };
                const D3D12_ROOT_SIGNATURE_DESC rs{
                    4, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
                winrt::com_ptr<ID3DBlob> serialized, errors;
                DX::ThrowIfFailed(D3D12SerializeRootSignature(
                    &rs, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
                DX::ThrowIfFailed(device->CreateRootSignature(0,
                    serialized->GetBufferPointer(), serialized->GetBufferSize(),
                    IID_PPV_ARGS(guideRoot.put())));
                D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
                state.pRootSignature = guideRoot.get();
                state.CS = { code.data(), code.size() };
                DX::ThrowIfFailed(device->CreateComputePipelineState(
                    &state, IID_PPV_ARGS(guidePipeline.put())));
                const D3D12_DESCRIPTOR_HEAP_DESC heap{
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                    kRRGuideDescriptorsPerFrame * 2,
                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
                DX::ThrowIfFailed(device->CreateDescriptorHeap(
                    &heap, IID_PPV_ARGS(guideDescriptors.put())));
            }

            const auto size = frame.sampling.logical;
            if (frame.rrGuideSize != size) {
                for (auto& guide : frame.rrGuides) guide = nullptr;
                frame.rrGuideSize = size;
            }
            for (uint32_t i = 0; i < frame.rrGuides.size(); ++i) {
                if (frame.rrGuides[i]) continue;
                const DXGI_FORMAT format = i == 0 ?
                    DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
                const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                    format, size[0], size[1], 1, 1, 1, 0,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
                DX::ThrowIfFailed(device->CreateCommittedResource(&heap,
                    D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                    nullptr, IID_PPV_ARGS(frame.rrGuides[i].put())));
            }

            struct alignas(16) GuideSettings {
                uint32_t nativeWidth, nativeHeight, logicalWidth, logicalHeight;
                uint32_t representativePhase, sampleScale, linearLighting, reserved;
            };
            static_assert(sizeof(GuideSettings) == 32);
            static_assert(kRRGuideOffset + sizeof(GuideSettings) <= kUploadBytes);
            const GuideSettings guideSettings{
                frame.sampling.native[0], frame.sampling.native[1],
                size[0], size[1], frame.sampling.representativePhase,
                uint32_t(frame.sampling.spatialScale),
                uint32_t(globals::features::linearLighting.IsLinearLightingActive()), 0u };
            uint8_t* mapped = nullptr;
            const D3D12_RANGE noRead{ 0, 0 };
            DX::ThrowIfFailed(frame.constants->Map(0, &noRead,
                reinterpret_cast<void**>(&mapped)));
            std::memcpy(mapped + kRRGuideOffset, &guideSettings, sizeof(guideSettings));
            frame.constants->Unmap(0, nullptr);

            const std::array<ID3D12Resource*, 5> reads{
                linearDepth, normalRough, motion, diffuseAlbedo, specularAlbedo };
            const std::array<DXGI_FORMAT, 5> formats{
                DXGI_FORMAT_R32_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT };
            const UINT step = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto cpu = guideDescriptors->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += uint64_t(slot * kRRGuideDescriptorsPerFrame) * step;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            for (uint32_t i = 0; i < reads.size(); ++i) {
                srv.Format = formats[i];
                device->CreateShaderResourceView(reads[i], &srv, cpu);
                cpu.ptr += step;
            }
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            for (uint32_t i = 0; i < frame.rrGuides.size(); ++i) {
                uav.Format = formats[i];
                device->CreateUnorderedAccessView(frame.rrGuides[i].get(),
                    nullptr, &uav, cpu);
                cpu.ptr += step;
            }
            std::array<D3D12_RESOURCE_BARRIER, 10> before{}, after{};
            for (uint32_t i = 0; i < reads.size(); ++i) {
                before[i] = CD3DX12_RESOURCE_BARRIER::Transition(reads[i],
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                after[i] = CD3DX12_RESOURCE_BARRIER::Transition(reads[i],
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COMMON);
                before[i + 5] = CD3DX12_RESOURCE_BARRIER::Transition(
                    frame.rrGuides[i].get(), D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                after[i + 5] = CD3DX12_RESOURCE_BARRIER::Transition(
                    frame.rrGuides[i].get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COMMON);
            }

            list->ResourceBarrier(UINT(before.size()), before.data());
            ID3D12DescriptorHeap* heaps[]{ guideDescriptors.get() };
            list->SetDescriptorHeaps(1, heaps);
            list->SetPipelineState(guidePipeline.get());
            list->SetComputeRootSignature(guideRoot.get());
            auto gpu = guideDescriptors->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += uint64_t(slot * kRRGuideDescriptorsPerFrame) * step;
            list->SetComputeRootDescriptorTable(0, gpu);
            gpu.ptr += reads.size() * step;
            list->SetComputeRootDescriptorTable(1, gpu);
            list->SetComputeRootConstantBufferView(2,
                frame.constants->GetGPUVirtualAddress() + kRRGuideOffset);
            list->SetComputeRootConstantBufferView(3,
                frame.constants->GetGPUVirtualAddress() + kCameraOffset);
            list->Dispatch((size[0] + 7u) / 8u, (size[1] + 7u) / 8u, 1);
            list->ResourceBarrier(UINT(after.size()), after.data());
            frame.rrGuidesValid = true;
            ++guideDispatches;
            guideFailure.clear();
            return true;
        } catch (const std::exception& error) {
            guideFailure = error.what();
            frame.rrGuidesValid = false;
            return false;
        }
    }



    bool NativeHybridGI::RecordHistoryValidation(ID3D12Device* device,
        ID3D12GraphicsCommandList* list, FrameOutputs& current,
        uint32_t slot, uint32_t frameNumber, uint64_t sceneGeneration)
    {
        current.rrHistoryValid = false;
        if (!device || !list || !current.rrGuidesValid || slot >= 2u) {
            historyFailure = "Current native RR guide frame unavailable";
            return false;
        }
        try {
            if (!historyPipeline || !historyRoot || !historyDescriptors) {
                historyPipeline = nullptr; historyRoot = nullptr;
                historyDescriptors = nullptr;
                const auto code = ReadDXIL(
                    "Data/Shaders/RTDirectLighting/nativeHybridRRHistory.dxil");
                D3D12_DESCRIPTOR_RANGE ranges[2]{};
                ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0 };
                ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
                D3D12_ROOT_PARAMETER params[3]{};
                for (uint32_t i = 0; i < 2; ++i) {
                    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                    params[i].DescriptorTable = { 1, &ranges[i] };
                }
                params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                params[2].Constants = { 0, 0, 4 };
                const D3D12_ROOT_SIGNATURE_DESC rs{
                    3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
                winrt::com_ptr<ID3DBlob> serialized, errors;
                DX::ThrowIfFailed(D3D12SerializeRootSignature(&rs,
                    D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
                DX::ThrowIfFailed(device->CreateRootSignature(0,
                    serialized->GetBufferPointer(), serialized->GetBufferSize(),
                    IID_PPV_ARGS(historyRoot.put())));
                D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
                state.pRootSignature = historyRoot.get();
                state.CS = { code.data(), code.size() };
                DX::ThrowIfFailed(device->CreateComputePipelineState(
                    &state, IID_PPV_ARGS(historyPipeline.put())));
                const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                    6u * 2u, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
                DX::ThrowIfFailed(device->CreateDescriptorHeap(
                    &heapDesc, IID_PPV_ARGS(historyDescriptors.put())));
            }
            const auto size = current.sampling.logical;
            if (historySize != size) {
                for (auto& previousGuide : historyGuides) previousGuide = nullptr;
                historySize = size;
                historyLastFrame = UINT32_MAX;
            }
            const bool compatible = historyGuides[0] && historyGuides[1] &&
                historyLastFrame != UINT32_MAX &&
                historyLastFrame + 1u == frameNumber &&
                historyGeneration == sceneGeneration &&
                historyMode == current.sampling.effective &&
                historyScale == current.sampling.spatialScale;
            for (uint32_t guideIndex = 0; guideIndex < 2; ++guideIndex) {
                if (historyGuides[guideIndex]) continue;
                const auto format = guideIndex == 0 ?
                    DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
                const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format,
                    size[0], size[1], 1, 1, 1, 0,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
                DX::ThrowIfFailed(device->CreateCommittedResource(&heap,
                    D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                    nullptr, IID_PPV_ARGS(historyGuides[guideIndex].put())));
            }
            if (!current.rrValidatedMotion ||
                current.rrValidatedMotion->GetDesc().Width != size[0] ||
                current.rrValidatedMotion->GetDesc().Height != size[1]) {
                current.rrValidatedMotion = nullptr;
                const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                    DXGI_FORMAT_R16G16B16A16_FLOAT,
                    size[0], size[1], 1, 1, 1, 0,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
                DX::ThrowIfFailed(device->CreateCommittedResource(&heap,
                    D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                    nullptr, IID_PPV_ARGS(current.rrValidatedMotion.put())));
            }
            // All future reads come from persistent snapshots, never the
            // two independently recyclable scene frame slots.
            const std::array<ID3D12Resource*, 5> inputs{
                current.rrGuides[0].get(), current.rrGuides[1].get(),
                current.rrGuides[2].get(), historyGuides[0].get(),
                historyGuides[1].get()
            };
            const std::array<DXGI_FORMAT, 5> formats{
                DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT
            };
            const auto step = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto cpu = historyDescriptors->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += uint64_t(slot * 6u) * step;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            for (uint32_t i = 0; i < inputs.size(); ++i) {
                srv.Format = formats[i];
                device->CreateShaderResourceView(inputs[i], &srv, cpu);
                cpu.ptr += step;
            }
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            device->CreateUnorderedAccessView(
                current.rrValidatedMotion.get(), nullptr, &uav, cpu);
            if (compatible) {
            std::array<D3D12_RESOURCE_BARRIER, 6> begin{}, end{};
            for (uint32_t i = 0; i < inputs.size(); ++i) {
                begin[i] = CD3DX12_RESOURCE_BARRIER::Transition(inputs[i],
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                end[i] = CD3DX12_RESOURCE_BARRIER::Transition(inputs[i],
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COMMON);
            }
            begin[5] = CD3DX12_RESOURCE_BARRIER::Transition(
                current.rrValidatedMotion.get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            end[5] = CD3DX12_RESOURCE_BARRIER::Transition(
                current.rrValidatedMotion.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON);
            list->ResourceBarrier(UINT(begin.size()), begin.data());
            ID3D12DescriptorHeap* heaps[]{ historyDescriptors.get() };
            list->SetDescriptorHeaps(1, heaps);
            list->SetPipelineState(historyPipeline.get());
            list->SetComputeRootSignature(historyRoot.get());
            auto gpu = historyDescriptors->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += uint64_t(slot * 6u) * step;
            list->SetComputeRootDescriptorTable(0, gpu);
            gpu.ptr += 5u * step;
            list->SetComputeRootDescriptorTable(1, gpu);
            struct HistoryConstants {
                uint32_t width, height;
                float depthTolerance, normalThreshold;
            } constants{ size[0], size[1], 0.02f, 0.5f };
            static_assert(sizeof(constants) == 16);
            list->SetComputeRoot32BitConstants(2, 4, &constants, 0);
            list->Dispatch((size[0] + 7u) / 8u, (size[1] + 7u) / 8u, 1);
            list->ResourceBarrier(UINT(end.size()), end.data());
            current.rrHistoryValid = true;
            ++historyValidationDispatches;
            } // compatible persistent history

            // Capture the current logical depth and surface normal for the
            // following frame. These copies stay on the same DX12 queue;
            // there is no CPU readback, D3D11 handoff, or GPU-stalling wait.
            for (uint32_t i = 0; i < 2; ++i) {
                auto* src = current.rrGuides[i == 0 ? 0 : 2].get();
                auto* dst = historyGuides[i].get();
                const std::array<D3D12_RESOURCE_BARRIER, 2> before{
                    CD3DX12_RESOURCE_BARRIER::Transition(src,
                        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                    CD3DX12_RESOURCE_BARRIER::Transition(dst,
                        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)
                };
                list->ResourceBarrier(UINT(before.size()), before.data());
                list->CopyResource(dst, src);
                const std::array<D3D12_RESOURCE_BARRIER, 2> after{
                    CD3DX12_RESOURCE_BARRIER::Transition(src,
                        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
                    CD3DX12_RESOURCE_BARRIER::Transition(dst,
                        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)
                };
                list->ResourceBarrier(UINT(after.size()), after.data());
            }
            historyLastFrame = frameNumber;
            historyGeneration = sceneGeneration;
            historyMode = current.sampling.effective;
            historyScale = current.sampling.spatialScale;
            historyFailure = compatible ? "" :
                "Temporal history reset: first frame, resize, mode or scene transition";
            return current.rrHistoryValid;
        } catch (const std::exception& error) {
            current.rrHistoryValid = false;
            historyFailure = error.what();
            return false;
        }
    }


    bool NativeHybridGI::RecordNativeReconstruction(ID3D12Device* device,
        ID3D12GraphicsCommandList* list, FrameOutputs& frame, uint32_t slot,
        uint32_t fallbackFamilyMask)
    {
        if (!device || !list || slot >= 2u || !frame.constants ||
            !frame.rrGuides[0] || !frame.rrGuides[2] ||
            !frame.textures[Eligibility]) {
            postFailure = "Native reconstruction missing GPU guides";
            return false;
        }
        try {
            if (!nativeReconstructionRoot || !nativeReconstructionPipeline ||
                !nativeReconstructionDescriptors) {
                nativeReconstructionRoot = nullptr;
                nativeReconstructionPipeline = nullptr;
                nativeReconstructionDescriptors = nullptr;
                const auto code = ReadDXIL(
                    "Data/Shaders/RTDirectLighting/hybridNativeReconstruct.dxil");
                D3D12_DESCRIPTOR_RANGE ranges[2]{};
                ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0 };
                ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
                D3D12_ROOT_PARAMETER params[3]{};
                for (uint32_t i = 0; i < 2; ++i) {
                    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                    params[i].DescriptorTable = { 1, &ranges[i] };
                }
                params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                params[2].Descriptor = { 0, 0 };
                const D3D12_ROOT_SIGNATURE_DESC desc{
                    3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
                winrt::com_ptr<ID3DBlob> serialized, errors;
                DX::ThrowIfFailed(D3D12SerializeRootSignature(&desc,
                    D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
                DX::ThrowIfFailed(device->CreateRootSignature(0,
                    serialized->GetBufferPointer(), serialized->GetBufferSize(),
                    IID_PPV_ARGS(nativeReconstructionRoot.put())));
                D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
                state.pRootSignature = nativeReconstructionRoot.get();
                state.CS = { code.data(), code.size() };
                DX::ThrowIfFailed(device->CreateComputePipelineState(
                    &state, IID_PPV_ARGS(nativeReconstructionPipeline.put())));
                const D3D12_DESCRIPTOR_HEAP_DESC heap{
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                    kNativeDescriptorsPerFrame * 2u,
                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0u };
                DX::ThrowIfFailed(device->CreateDescriptorHeap(
                    &heap, IID_PPV_ARGS(nativeReconstructionDescriptors.put())));
            }
            struct alignas(16) Constants {
                uint32_t nativeWidth, nativeHeight, logicalWidth, logicalHeight;
                uint32_t packedWidth, packedHeight, family, advancedTier;
                uint32_t checkerboard, checkerboardOrigin, scale, phase;
                uint32_t radius, reserved[3]{};
            };
            static_assert(sizeof(Constants) == 64);
            static_assert(kNativeReconstructionOffset + 4u * 256u <= kUploadBytes);
            uint8_t* mapped = nullptr;
            const D3D12_RANGE noRead{ 0, 0 };
            DX::ThrowIfFailed(frame.constants->Map(0, &noRead,
                reinterpret_cast<void**>(&mapped)));
            const auto logical = frame.sampling.logical;
            const auto native = frame.sampling.native;
            for (uint32_t signal = 0; signal < 4u; ++signal) {
                const Constants constants{
                    native[0], native[1], logical[0], logical[1],
                    frame.sampling.packed[0], frame.sampling.packed[1],
                    std::min(signal, 2u), frame.advancedTransportTier,
                    uint32_t(frame.sampling.checkerboard),
                    frame.sampling.checkerboardOrigin,
                    frame.sampling.spatialScale, frame.sampling.representativePhase,
                    signal < 2u ? 4u : 2u };
                std::memcpy(mapped + kNativeReconstructionOffset + signal * 256,
                    &constants, sizeof(constants));
            }
            frame.constants->Unmap(0, nullptr);
            const UINT step = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            for (uint32_t signal = 0; signal < 4u; ++signal) {
                const DXGI_FORMAT format = signal < 2u ?
                    DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8_UNORM;
                const uint32_t inputIndex = signal == 0u ? Diffuse :
                    signal == 1u ? Specular :
                    signal == 2u ? DiffuseAO : SpecularAO;
                const std::array<ID3D12Resource*, 4> inputs{
                    frame.textures[inputIndex].get(), frame.rrGuides[0].get(),
                    frame.rrGuides[2].get(), frame.textures[Eligibility].get() };
                const std::array<DXGI_FORMAT, 4> formats{
                    format, DXGI_FORMAT_R32_FLOAT,
                    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_UINT };
                auto cpu = nativeReconstructionDescriptors->GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += uint64_t(slot * kNativeDescriptorsPerFrame +
                    signal * kNativeDescriptorsPerSignal) * step;
                for (uint32_t i = 0; i < inputs.size(); ++i) {
                    srv.Format = formats[i];
                    device->CreateShaderResourceView(inputs[i], &srv, cpu);
                    cpu.ptr += step;
                }
                uav.Format = format;
                device->CreateUnorderedAccessView(frame.rrOutputs[signal].get(),
                    nullptr, &uav, cpu);
                // Unrequested families retain their successful AMD RR outputs.
                // AO and SO share the same SDK context and fallback atomically.
                const uint32_t family = std::min(signal, 2u);
                if ((fallbackFamilyMask & (1u << family)) == 0u)
                    continue;
                std::array<D3D12_RESOURCE_BARRIER, 5> begin{}, end{};
                for (uint32_t i = 0; i < inputs.size(); ++i) {
                    begin[i] = CD3DX12_RESOURCE_BARRIER::Transition(inputs[i],
                        D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    end[i] = CD3DX12_RESOURCE_BARRIER::Transition(inputs[i],
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COMMON);
                }
                begin[4] = CD3DX12_RESOURCE_BARRIER::Transition(
                    frame.rrOutputs[signal].get(), D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                end[4] = CD3DX12_RESOURCE_BARRIER::Transition(
                    frame.rrOutputs[signal].get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COMMON);
                list->ResourceBarrier(UINT(begin.size()), begin.data());
                ID3D12DescriptorHeap* heaps[]{ nativeReconstructionDescriptors.get() };
                list->SetDescriptorHeaps(1, heaps);
                list->SetComputeRootSignature(nativeReconstructionRoot.get());
                list->SetPipelineState(nativeReconstructionPipeline.get());
                auto gpu = nativeReconstructionDescriptors->GetGPUDescriptorHandleForHeapStart();
                gpu.ptr += uint64_t(slot * kNativeDescriptorsPerFrame +
                    signal * kNativeDescriptorsPerSignal) * step;
                list->SetComputeRootDescriptorTable(0, gpu);
                gpu.ptr += 4u * step;
                list->SetComputeRootDescriptorTable(1, gpu);
                list->SetComputeRootConstantBufferView(2,
                    frame.constants->GetGPUVirtualAddress() +
                    kNativeReconstructionOffset + signal * 256u);
                list->Dispatch((logical[0] + 7u) / 8u,
                    (logical[1] + 7u) / 8u, 1u);
                list->ResourceBarrier(UINT(end.size()), end.data());
                ++nativeReconstructionDispatches;
            }
            return true;
        } catch (const std::exception& error) {
            postFailure = std::string("Native reconstruction: ") + error.what();
            return false;
        }
    }


    bool NativeHybridGI::RecordNativeTemporal(ID3D12Device* device,
        ID3D12GraphicsCommandList* list, FrameOutputs& frame,
        uint32_t slot, uint32_t frameNumber, uint32_t fallbackFamilyMask)
    {
        if (!device || !list || slot >= 2u || !frame.constants ||
            !frame.rrGuidesValid || !frame.textures[Eligibility]) {
            postFailure = "Native temporal reconstruction missing current GPU guides";
            return false;
        }
        try {
            if (!temporalRoot || !temporalPipeline || !temporalDescriptors) {
                temporalRoot = nullptr;
                temporalPipeline = nullptr;
                temporalDescriptors = nullptr;
                const auto code = ReadDXIL(
                    "Data/Shaders/RTDirectLighting/hybridNativeTemporal.dxil");
                D3D12_DESCRIPTOR_RANGE ranges[2]{};
                ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0 };
                ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
                D3D12_ROOT_PARAMETER params[3]{};
                for (uint32_t i = 0; i < 2u; ++i) {
                    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                    params[i].DescriptorTable = { 1, &ranges[i] };
                }
                params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                params[2].Descriptor = { 0, 0 };
                const D3D12_ROOT_SIGNATURE_DESC desc{
                    3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
                winrt::com_ptr<ID3DBlob> serialized, errors;
                DX::ThrowIfFailed(D3D12SerializeRootSignature(&desc,
                    D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
                DX::ThrowIfFailed(device->CreateRootSignature(0,
                    serialized->GetBufferPointer(), serialized->GetBufferSize(),
                    IID_PPV_ARGS(temporalRoot.put())));
                D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
                state.pRootSignature = temporalRoot.get();
                state.CS = { code.data(), code.size() };
                DX::ThrowIfFailed(device->CreateComputePipelineState(
                    &state, IID_PPV_ARGS(temporalPipeline.put())));
                const D3D12_DESCRIPTOR_HEAP_DESC heap{
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                    kNativeTemporalDescriptorsPerFrame * 2u,
                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
                DX::ThrowIfFailed(device->CreateDescriptorHeap(
                    &heap, IID_PPV_ARGS(temporalDescriptors.put())));
            }
            const auto logical = frame.sampling.logical;
            if (nativeHistorySize != logical) {
                // Enforce a hard cap on quarantined history generations.
                // When exceeded, keep the correct spatial reconstruction
                // rather than allocating unbounded VRAM or releasing an
                // in-flight command-list resource.
                size_t retiring = 0u;
                for (const auto& bank : nativeTemporalHistory)
                    for (const auto& resource : bank)
                        if (resource) ++retiring;
                constexpr size_t kMaxRetiredNativeHistoryTextures = 16u;
                if (retiredNativeHistory.size() + retiring >
                    kMaxRetiredNativeHistoryTextures) {
                    temporalFailure = "History resize budget exhausted; safe spatial reconstruction retained";
                    return true;
                }
                for (auto& bank : nativeTemporalHistory)
                    for (auto& resource : bank) {
                        if (!resource) continue;
                        const auto desc = resource->GetDesc();
                        const auto bytes = device->GetResourceAllocationInfo(
                            0, 1, &desc).SizeInBytes;
                        if (bytes != UINT64_MAX &&
                            retiredNativeHistoryBytes <= UINT64_MAX - bytes)
                            retiredNativeHistoryBytes += bytes;
                        retiredNativeHistory.push_back(std::move(resource));
                    }
                nativeHistorySize = logical;
                nativeHistoryLastFrame.fill(UINT32_MAX);
            }
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const uint32_t currentBank = frameNumber & 1u;
            const uint32_t previousBank = currentBank ^ 1u;
            struct alignas(16) Constants {
                uint32_t logicalWidth, logicalHeight, nativeWidth, nativeHeight;
                uint32_t signalFamily, scale, phase, historyAvailable;
                float blend;
                float padding[3]{};
            };
            static_assert(sizeof(Constants) == 48);
            static_assert(kNativeTemporalOffset + 4u * 256u <= kUploadBytes);
            uint8_t* mapped = nullptr;
            const D3D12_RANGE noRead{ 0, 0 };
            DX::ThrowIfFailed(frame.constants->Map(0, &noRead,
                reinterpret_cast<void**>(&mapped)));
            for (uint32_t signal = 0; signal < 4u; ++signal) {
                const uint32_t family = std::min(signal, 2u);
                if ((fallbackFamilyMask & (1u << family)) == 0u) continue;
                const DXGI_FORMAT format = signal < 2u ?
                    DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8_UNORM;
                const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format,
                    logical[0], logical[1], 1, 1, 1, 0,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                for (auto& bank : nativeTemporalHistory) {
                    if (bank[signal]) continue;
                    DX::ThrowIfFailed(device->CreateCommittedResource(&heap,
                        D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                        nullptr, IID_PPV_ARGS(bank[signal].put())));
                }
                const bool historyAvailable = frame.rrHistoryValid &&
                    nativeHistoryLastFrame[signal] != UINT32_MAX &&
                    nativeHistoryLastFrame[signal] + 1u == frameNumber;
                const Constants constants{ logical[0], logical[1],
                    frame.sampling.native[0], frame.sampling.native[1], family,
                    frame.sampling.spatialScale, frame.sampling.representativePhase,
                    uint32_t(historyAvailable), 0.75f };
                std::memcpy(mapped + kNativeTemporalOffset + signal * 256u,
                    &constants, sizeof(constants));
            }
            frame.constants->Unmap(0, nullptr);

            const UINT step = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            const auto motion = frame.rrValidatedMotion ?
                frame.rrValidatedMotion.get() : frame.rrGuides[1].get();
            for (uint32_t signal = 0; signal < 4u; ++signal) {
                const uint32_t family = std::min(signal, 2u);
                if ((fallbackFamilyMask & (1u << family)) == 0u) continue;
                const DXGI_FORMAT format = signal < 2u ?
                    DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8_UNORM;
                const std::array<ID3D12Resource*, 4> inputs{
                    frame.rrOutputs[signal].get(),
                    nativeTemporalHistory[previousBank][signal].get(),
                    motion, frame.textures[Eligibility].get() };
                const std::array<DXGI_FORMAT, 4> formats{
                    format, format, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_UINT };
                auto cpu = temporalDescriptors->GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += uint64_t(slot * kNativeTemporalDescriptorsPerFrame +
                    signal * kNativeTemporalDescriptorsPerSignal) * step;
                for (uint32_t i = 0; i < inputs.size(); ++i) {
                    srv.Format = formats[i];
                    device->CreateShaderResourceView(inputs[i], &srv, cpu);
                    cpu.ptr += step;
                }
                uav.Format = format;
                device->CreateUnorderedAccessView(
                    nativeTemporalHistory[currentBank][signal].get(),
                    nullptr, &uav, cpu);
                std::array<D3D12_RESOURCE_BARRIER, 5> begin{}, end{};
                for (uint32_t i = 0; i < inputs.size(); ++i) {
                    begin[i] = CD3DX12_RESOURCE_BARRIER::Transition(inputs[i],
                        D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    end[i] = CD3DX12_RESOURCE_BARRIER::Transition(inputs[i],
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COMMON);
                }
                begin[4] = CD3DX12_RESOURCE_BARRIER::Transition(
                    nativeTemporalHistory[currentBank][signal].get(),
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                end[4] = CD3DX12_RESOURCE_BARRIER::Transition(
                    nativeTemporalHistory[currentBank][signal].get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                list->ResourceBarrier(UINT(begin.size()), begin.data());
                ID3D12DescriptorHeap* heaps[]{ temporalDescriptors.get() };
                list->SetDescriptorHeaps(1, heaps);
                list->SetComputeRootSignature(temporalRoot.get());
                list->SetPipelineState(temporalPipeline.get());
                auto gpu = temporalDescriptors->GetGPUDescriptorHandleForHeapStart();
                gpu.ptr += uint64_t(slot * kNativeTemporalDescriptorsPerFrame +
                    signal * kNativeTemporalDescriptorsPerSignal) * step;
                list->SetComputeRootDescriptorTable(0, gpu);
                gpu.ptr += 4u * step;
                list->SetComputeRootDescriptorTable(1, gpu);
                list->SetComputeRootConstantBufferView(2,
                    frame.constants->GetGPUVirtualAddress() +
                    kNativeTemporalOffset + signal * 256u);
                list->Dispatch((logical[0] + 7u) / 8u,
                    (logical[1] + 7u) / 8u, 1u);
                list->ResourceBarrier(UINT(end.size()), end.data());
                frame.nativeTemporalFinal[signal] =
                    nativeTemporalHistory[currentBank][signal].get();
                nativeHistoryLastFrame[signal] = frameNumber;
                ++nativeTemporalDispatches;
            }
            temporalFailure.clear();
            return true;
        } catch (const std::exception& error) {
            temporalFailure = error.what();
            postFailure = std::string("Native temporal reconstruction: ") + error.what();
            return false;
        }
    }

    bool NativeHybridGI::RecordPostRR(ID3D12Device* device,
        ID3D12GraphicsCommandList* list, FrameOutputs& frame, uint32_t slot,
        uint32_t frameNumber, ID3D12Resource* nativeDepth,
        ID3D12Resource* nativeNormal, const RTComposer::CompositionSettings& policy,
        RedstoneDenoiser* denoiser, bool requestRR)
    {
        frame.rrValid = frame.postValid = false;
        frame.nativeValidityMask = 0u;
        frame.rrFamilySuccessMask = frame.nativeFamilyFallbackMask = 0u;
        reconstructionProvider = "none";
        reconstructionFallbackReason.clear();
        frame.nativeFinal.fill(nullptr);
        frame.nativeTemporalFinal.fill(nullptr);
        if (!device || !list || !frame.valid || !frame.rrGuidesValid ||
            !nativeDepth || !nativeNormal || !frame.textures[Eligibility] ||
            !frame.constants || slot >= 2) {
            postFailure = "Missing native GI, RR guides, or post-processing inputs";
            return false;
        }
        try {
            const auto logical = frame.sampling.logical;
            const auto native = frame.sampling.native;
            if (frame.rrOutputSize != logical || frame.postNativeSize != native) {
                for (auto& resource : frame.rrOutputs) resource = nullptr;
                for (auto& resource : frame.nativeFiltered) resource = nullptr;
                for (auto& resource : frame.nativeBlurred) resource = nullptr;
                frame.rrOutputSize = logical;
                frame.postNativeSize = native;
            }
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            for (uint32_t i = 0; i < 4; ++i) {
                const DXGI_FORMAT format = i < 2 ?
                    DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8_UNORM;
                std::array<winrt::com_ptr<ID3D12Resource>*, 3> targets{
                    &frame.rrOutputs[i], &frame.nativeFiltered[i], &frame.nativeBlurred[i] };
                for (uint32_t kind = 0; kind < targets.size(); ++kind) {
                    // Optional blur scratch is released on the retired frame
                    // slot when the control is off. It is not a permanent
                    // four-signal native-resolution allocation.
                    if (kind == 2u &&
                        !policy.reconstruction[std::min(i, 2u)].blurEnabled) {
                        *targets[kind] = nullptr;
                        continue;
                    }
                    if (*targets[kind]) continue;
                    const auto size = kind == 0 ? logical : native;
                    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                        format, size[0], size[1], 1, 1, 1, 0,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                    DX::ThrowIfFailed(device->CreateCommittedResource(&heap,
                        D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                        nullptr, IID_PPV_ARGS(targets[kind]->put())));
                }
            }

            RedstoneDenoiser::NativeHybridResources resources{};
            for (uint32_t i = 0; i < 4; ++i) {
                const uint32_t inputIndex = i == 0 ? Diffuse :
                    i == 1 ? Specular : i == 2 ? DiffuseAO : SpecularAO;
                resources.inputs[i] = frame.textures[inputIndex].get();
                resources.outputs[i] = frame.rrOutputs[i].get();
            }
            for (uint32_t i = 0; i < resources.guides.size(); ++i)
                resources.guides[i] = frame.rrGuides[i].get();
            if (frame.rrHistoryValid && frame.rrValidatedMotion)
                resources.guides[1] = frame.rrValidatedMotion.get();
            resources.resetHistory = !frame.rrHistoryValid;
            resources.skipOcclusion = frame.advancedTransportTier != 0u;
            resources.diffuseOnly = frame.advancedTransportTier == 2u;
            resources.nativeSize = native;
            resources.logicalSize = logical;
            resources.frame = frameNumber;
            resources.checkerboard = frame.sampling.checkerboard;
            resources.checkerboardOrigin = frame.sampling.checkerboardOrigin;
            const uint32_t activeMask = frame.advancedTransportTier == 2u ? 1u :
                frame.advancedTransportTier != 0u ? 3u : 7u;
            if (requestRR && denoiser) {
                (void)denoiser->DispatchNativeHybridRR(list, resources);
                frame.rrFamilySuccessMask =
                    denoiser->NativeHybridSuccessfulFamilyMask() & activeMask;
            }
            frame.nativeFamilyFallbackMask =
                activeMask & ~frame.rrFamilySuccessMask;
            frame.rrValid = frame.rrFamilySuccessMask == activeMask;
            if (frame.nativeFamilyFallbackMask != 0u) {
                reconstructionFallbackReason = requestRR ?
                    (denoiser ? denoiser->NativeHybridFailure() :
                        "FSR RR provider unavailable") : "FSR RR disabled by policy";
                if (!RecordNativeReconstruction(device, list, frame, slot,
                        frame.nativeFamilyFallbackMask))
                    return false;
                if (!RecordNativeTemporal(device, list, frame, slot,
                        frameNumber, frame.nativeFamilyFallbackMask))
                    return false;
                frame.nativeValidityMask = frame.nativeFamilyFallbackMask & 3u;
            }
            reconstructionProvider = frame.rrValid ? "fsr-rr" :
                frame.rrFamilySuccessMask ? "mixed-fsr-rr-native-dx12" :
                "native-dx12";

            if (!postRoot || !postPipelines[0] || !postPipelines[1] || !postDescriptors) {
                postRoot = nullptr; postPipelines[0] = nullptr;
                postPipelines[1] = nullptr; postDescriptors = nullptr;
                auto reconstructionCode = ReadDXIL(
                    "Data/Shaders/RTDirectLighting/hybridPostRRReconstruct.dxil");
                auto blurCode = ReadDXIL(
                    "Data/Shaders/RTDirectLighting/hybridPostRRBlur.dxil");
                D3D12_DESCRIPTOR_RANGE ranges[2]{};
                ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0 };
                ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
                D3D12_ROOT_PARAMETER params[3]{};
                for (uint32_t i = 0; i < 2; ++i) {
                    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                    params[i].DescriptorTable = { 1, &ranges[i] };
                }
                params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                params[2].Descriptor = { 0, 0 };
                const D3D12_ROOT_SIGNATURE_DESC rs{
                    3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
                winrt::com_ptr<ID3DBlob> serialized, errors;
                DX::ThrowIfFailed(D3D12SerializeRootSignature(&rs,
                    D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
                DX::ThrowIfFailed(device->CreateRootSignature(0,
                    serialized->GetBufferPointer(), serialized->GetBufferSize(),
                    IID_PPV_ARGS(postRoot.put())));
                for (uint32_t pass = 0; pass < 2; ++pass) {
                    const auto& code = pass ? blurCode : reconstructionCode;
                    D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
                    state.pRootSignature = postRoot.get();
                    state.CS = { code.data(), code.size() };
                    DX::ThrowIfFailed(device->CreateComputePipelineState(
                        &state, IID_PPV_ARGS(postPipelines[pass].put())));
                }
                const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                    kPostPerFrame * 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
                DX::ThrowIfFailed(device->CreateDescriptorHeap(
                    &heapDesc, IID_PPV_ARGS(postDescriptors.put())));
            }

            // Both constant layouts fit a 64-byte per-pass slot, with DX12's
            // required 256-byte CBV address alignment handled below.
            struct alignas(16) ReconstructCB {
                uint32_t width, height, logicalWidth, logicalHeight;
                uint32_t family, filter, sampleScale, phase;
                float radius, strength, depth, normal, roughness;
                uint32_t nativeProvider = 0u;
                float padding[2]{};
            };
            struct alignas(16) BlurCB {
                uint32_t width, height, family, enabled;
                float radius, strength, depth, normal, roughness;
                uint32_t nativeProvider = 0u;
                float padding[2]{};
            };
            static_assert(sizeof(ReconstructCB) == 64);
            static_assert(sizeof(BlurCB) == 48);
            static_assert(kPostOffset + (4 * 2) * 256 <= kUploadBytes,
                "Native post-RR CB arena exceeds frame upload");
            uint8_t* mapped = nullptr;
            const D3D12_RANGE noRead{ 0, 0 };
            DX::ThrowIfFailed(frame.constants->Map(0, &noRead,
                reinterpret_cast<void**>(&mapped)));
            for (uint32_t signal = 0; signal < 4; ++signal) {
                const uint32_t family = std::min(signal, 2u);
                const auto& settings = policy.reconstruction[family];
                const ReconstructCB reconstruct{
                    native[0], native[1], logical[0], logical[1], family,
                    uint32_t(settings.filterEnabled), uint32_t(frame.sampling.spatialScale),
                    frame.sampling.representativePhase,
                    settings.filterRadius, settings.filterStrength,
                    settings.depthSensitivity, settings.normalSensitivity,
                    settings.roughnessSensitivity,
                    frame.nativeValidityMask != 0u ? 1u : 0u };
                const BlurCB blur{
                    native[0], native[1], family, uint32_t(settings.blurEnabled),
                    settings.blurRadius, settings.blurStrength,
                    settings.depthSensitivity, settings.normalSensitivity,
                    settings.roughnessSensitivity,
                    frame.nativeValidityMask != 0u ? 1u : 0u };
                std::memcpy(mapped + kPostOffset + (signal * 2) * 256,
                    &reconstruct, sizeof(reconstruct));
                std::memcpy(mapped + kPostOffset + (signal * 2 + 1) * 256,
                    &blur, sizeof(blur));
            }
            frame.constants->Unmap(0, nullptr);

            const UINT step = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            for (uint32_t signal = 0; signal < 4; ++signal) {
                const DXGI_FORMAT format = signal < 2 ?
                    DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8_UNORM;
                for (uint32_t pass = 0; pass < 2; ++pass) {
                    const std::array<ID3D12Resource*, 4> inputs{
                        pass ? frame.nativeFiltered[signal].get() :
                            (frame.nativeTemporalFinal[signal] ?
                                frame.nativeTemporalFinal[signal] :
                                frame.rrOutputs[signal].get()),
                        nativeDepth, nativeNormal, frame.textures[Eligibility].get()
                    };
                    const std::array<DXGI_FORMAT, 4> formats{
                        format, DXGI_FORMAT_R32_FLOAT,
                        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_UINT
                    };
                    auto cpu = postDescriptors->GetCPUDescriptorHandleForHeapStart();
                    cpu.ptr += uint64_t(slot * kPostPerFrame +
                        (signal * 2 + pass) * kPostPerPass) * step;
                    for (uint32_t i = 0; i < inputs.size(); ++i) {
                        srv.Format = formats[i];
                        device->CreateShaderResourceView(inputs[i], &srv, cpu);
                        cpu.ptr += step;
                    }
                    uav.Format = format;
                    device->CreateUnorderedAccessView(
                        pass ? frame.nativeBlurred[signal].get() :
                            frame.nativeFiltered[signal].get(), nullptr, &uav, cpu);
                }
            }
            ID3D12DescriptorHeap* heaps[]{ postDescriptors.get() };
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(postRoot.get());
            for (uint32_t signal = 0; signal < 4; ++signal) {
                if (frame.advancedTransportTier != 0u &&
                    (signal >= 2u || (frame.advancedTransportTier == 2u && signal == 1u))) {
                    // Tier I has no specular samples; all advanced tiers
                    // have deterministic neutral AO/SO. No RR or post-filter
                    // is recorded for these inactive families.
                    frame.nativeFinal[signal] = frame.textures[
                        signal == 1u ? Specular :
                        signal == 2u ? DiffuseAO : SpecularAO].get();
                    continue;
                }
                const auto& settings = policy.reconstruction[std::min(signal, 2u)];
                const uint32_t passes = settings.blurEnabled ? 2u : 1u;
                for (uint32_t pass = 0; pass < passes; ++pass) {
                    ID3D12Resource* input = pass ?
                        frame.nativeFiltered[signal].get() :
                        (frame.nativeTemporalFinal[signal] ?
                            frame.nativeTemporalFinal[signal] :
                            frame.rrOutputs[signal].get());
                    ID3D12Resource* output = pass ?
                        frame.nativeBlurred[signal].get() : frame.nativeFiltered[signal].get();
                    std::array<ID3D12Resource*, 4> reads{
                        input, nativeDepth, nativeNormal, frame.textures[Eligibility].get() };
                    std::array<D3D12_RESOURCE_BARRIER, 5> begin{}, end{};
                    for (uint32_t i = 0; i < reads.size(); ++i) {
                        begin[i] = CD3DX12_RESOURCE_BARRIER::Transition(reads[i],
                            D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        end[i] = CD3DX12_RESOURCE_BARRIER::Transition(reads[i],
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COMMON);
                    }
                    begin[4] = CD3DX12_RESOURCE_BARRIER::Transition(output,
                        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    end[4] = CD3DX12_RESOURCE_BARRIER::Transition(output,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                    list->ResourceBarrier(UINT(begin.size()), begin.data());
                    list->SetPipelineState(postPipelines[pass].get());
                    auto gpu = postDescriptors->GetGPUDescriptorHandleForHeapStart();
                    gpu.ptr += uint64_t(slot * kPostPerFrame +
                        (signal * 2 + pass) * kPostPerPass) * step;
                    list->SetComputeRootDescriptorTable(0, gpu);
                    gpu.ptr += 4 * step;
                    list->SetComputeRootDescriptorTable(1, gpu);
                    list->SetComputeRootConstantBufferView(2,
                        frame.constants->GetGPUVirtualAddress() +
                        kPostOffset + (signal * 2 + pass) * 256);
                    list->Dispatch((native[0] + 7u) / 8u, (native[1] + 7u) / 8u, 1);
                    list->ResourceBarrier(UINT(end.size()), end.data());
                    if (pass == 0) ++postDispatches;
                    else ++blurDispatches;
                }
                frame.nativeFinal[signal] = settings.blurEnabled ?
                    frame.nativeBlurred[signal].get() :
                    frame.nativeFiltered[signal].get();
            }
            frame.postValid = true;
            lastRecordedRRFamilyMask = frame.rrFamilySuccessMask;
            lastRecordedNativeFamilyMask = frame.nativeFamilyFallbackMask;
            lastRecordedReconstructionFrame = frameNumber;
            postFailure.clear();
            return true;
        } catch (const std::exception& error) {
            frame.postValid = false;
            frame.nativeFinal.fill(nullptr);
            postFailure = error.what();
            return false;
        }
    }

    bool NativeHybridGI::RecordComposition(ID3D12Device* device, ID3D11Device5* device11,
        ID3D12GraphicsCommandList* list, FrameOutputs& frame, uint32_t slot,
        const std::array<uint32_t, 2>& size, ID3D12Resource* shadowedSource,
        ID3D12Resource* nativeDirect, ID3D12Resource* nativeShadowMask,
        ID3D12Resource* nativeLinearDepth, ID3D12Resource* diffuseAlbedo, ID3D12Resource* specularAlbedo,
        float directBlend, float giContribution, uint32_t shadowEdgeFilterEnabled,
        float shadowInfluence, uint32_t advancedTier)
    {
        frame.composedValid = false;
        if (!frame.valid || !device || !device11 || !list || !shadowedSource ||
            !nativeDirect || !nativeShadowMask || !nativeLinearDepth ||
            !diffuseAlbedo || !specularAlbedo ||
            slot >= 2 || !size[0] || !size[1] ||
            (!frame.postValid && frame.sampling.effective != HybridSamplingMode::Native)) {
            compositionFailure = "Missing valid GI/AO, source, or DX12 composition resources";
            return false;
        }
        try {
            // The advanced transporter has its own input PSO and can reuse
            // the composition backend without recording raster HybridGI.
            // Do not change the 9-SRV root shared by GI/AO/eligibility.
            // Composition consumes native visibility and depth for contact edge AA,
            // without modifying the root shared by GI/AO/eligibility.
            if (!compositionRoot) {
                D3D12_DESCRIPTOR_RANGE ranges[2]{};
                ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 11, 0, 0, 0 };
                ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0 };
                D3D12_ROOT_PARAMETER params[3]{};
                params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[0].DescriptorTable = { 1, &ranges[0] };
                params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[1].DescriptorTable = { 1, &ranges[1] };
                params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                params[2].Descriptor = { 13, 0 };
                const D3D12_ROOT_SIGNATURE_DESC signature{ 3, params, 0, nullptr,
                    D3D12_ROOT_SIGNATURE_FLAG_NONE };
                winrt::com_ptr<ID3DBlob> serialized, errors;
                DX::ThrowIfFailed(D3D12SerializeRootSignature(&signature,
                    D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
                DX::ThrowIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                    serialized->GetBufferSize(), IID_PPV_ARGS(compositionRoot.put())));
            }
            if (!compositionPipeline) {
                auto bytecode = ReadDXIL("Data/Shaders/RTDirectLighting/nativeHybridCompose.dxil");
                D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
                state.pRootSignature = compositionRoot.get();
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
                std::isfinite(giContribution) ? std::clamp(giContribution, 0.0f, 1.0f) : 0.0f,
                uint32_t(globals::features::linearLighting.IsLinearLightingActive()),
                uint32_t(globals::features::linearLighting.settings.enableACEScg),
                std::min(shadowEdgeFilterEnabled, 1u),
                std::isfinite(shadowInfluence) ? std::clamp(shadowInfluence, 0.0f, 1.0f) : 0.0f,
                advancedTier >= 2u && advancedTier <= 5u ? advancedTier : 0u,
                frame.nativeValidityMask };
            static_assert(sizeof(constants) == 40);
            static_assert(kCompositionOffset + sizeof(constants) <= kUploadBytes);
            uint8_t* mapped = nullptr;
            const D3D12_RANGE noRead{ 0, 0 };
            DX::ThrowIfFailed(frame.constants->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
            std::memcpy(mapped + kCompositionOffset, &constants, sizeof(constants));
            frame.constants->Unmap(0, nullptr);

            // Native post-RR outputs are full resolution. Sparse inputs must NEVER
            // be bound as if they were full-native textures.
            const bool reconstructed = frame.postValid;
            const std::array<ID3D12Resource*, 11> reads{
                shadowedSource, nativeDirect,
                reconstructed ? frame.nativeFinal[0] : frame.textures[Diffuse].get(),
                reconstructed ? frame.nativeFinal[1] : frame.textures[Specular].get(),
                reconstructed ? frame.nativeFinal[2] : frame.textures[DiffuseAO].get(),
                reconstructed ? frame.nativeFinal[3] : frame.textures[SpecularAO].get(),
                frame.textures[Eligibility].get(),
                diffuseAlbedo, specularAlbedo, nativeShadowMask, nativeLinearDepth
            };
            const std::array<DXGI_FORMAT, 11> formats{
                DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM,
                DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                DXGI_FORMAT_R32_FLOAT
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

            std::array<D3D12_RESOURCE_BARRIER, 12> begin{};
            std::array<D3D12_RESOURCE_BARRIER, 12> end{};
            for (uint32_t i = 0; i < reads.size(); ++i) {
                begin[i] = CD3DX12_RESOURCE_BARRIER::Transition(reads[i],
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                end[i] = CD3DX12_RESOURCE_BARRIER::Transition(reads[i],
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COMMON);
            }
            begin[11] = CD3DX12_RESOURCE_BARRIER::Transition(
                frame.composedScene->resource.get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            end[11] = CD3DX12_RESOURCE_BARRIER::Transition(
                frame.composedScene->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON);
            list->ResourceBarrier(UINT(begin.size()), begin.data());
            ID3D12DescriptorHeap* heaps[]{ compositionDescriptors.get() };
            list->SetDescriptorHeaps(1, heaps);
            list->SetPipelineState(compositionPipeline.get());
            list->SetComputeRootSignature(compositionRoot.get());
            auto gpu = compositionDescriptors->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += uint64_t(slot * kCompositionDescriptorsPerFrame) * step;
            list->SetComputeRootDescriptorTable(0, gpu);
            gpu.ptr += 11 * step;
            list->SetComputeRootDescriptorTable(1, gpu);
            list->SetComputeRootConstantBufferView(2,
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

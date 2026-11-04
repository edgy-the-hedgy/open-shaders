#include "NativeCompute.h"
#include "Globals.h"
#include "GpuPass.h"
#include <directx/d3dx12.h>
#include <fstream>
#include <set>

namespace DX12Native
{
    uint64_t NativeTextureBytes(ID3D12Device* device, std::array<uint32_t, 2> size, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
    {
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, size[0], size[1], 1, 1, 1, 0, flags);
        return device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    }
    uint64_t NativeAllocationBytes(ID3D12Device* device, ID3D12Resource* resource)
    {
        if (!resource) return 0;
        const auto desc = resource->GetDesc();
        return device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    }
    winrt::com_ptr<ID3D12Resource> CreateNativeTexture(ID3D12Device* device,
        std::array<uint32_t, 2> size, DXGI_FORMAT format, const wchar_t* name)
    {
        winrt::com_ptr<ID3D12Resource> result;
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, size[0], size[1], 1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        DX::ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(result.put())));
        result->SetName(name);
        return result;
    }

    void SnapshotNativeCamera(ID3D12Device* device, winrt::com_ptr<ID3D12Resource>& output)
    {
        if (!output) {
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            const auto desc = CD3DX12_RESOURCE_DESC::Buffer((sizeof(globals::FrameBuffer) + 255u) & ~255u);
            DX::ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(output.put())));
            output->SetName(L"DX12NativeScene::CameraSnapshot");
        }
        void* mapped = nullptr;
        const D3D12_RANGE noRead{ 0, 0 };
        DX::ThrowIfFailed(output->Map(0, &noRead, &mapped));
        std::memcpy(mapped, &globals::game::frameBufferCached.nonVR, sizeof(globals::FrameBuffer));
        output->Unmap(0, nullptr);
    }

    bool NativeCompute::Initialize(ID3D12Device* device, const char* file)
    {
        if (pipeline) return true;
        try {
            std::ifstream stream(file, std::ios::binary | std::ios::ate);
            if (!stream || stream.tellg() <= 0 || stream.tellg() > 16 * 1024 * 1024)
                throw std::runtime_error(std::string("Native pass DXIL unavailable: ") + file);
            std::vector<char> code(size_t(stream.tellg()));
            stream.seekg(0);
            if (!stream.read(code.data(), std::streamsize(code.size()))) throw std::runtime_error("Native DXIL read failed");
            const D3D12_DESCRIPTOR_RANGE ranges[]{
                { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 14, 0, 0, 0 },
                { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 8, 0, 0, 0 } };
            D3D12_ROOT_PARAMETER parameters[4]{};
            for (uint32_t i = 0; i < 2; ++i) {
                parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameters[i].DescriptorTable = { 1, &ranges[i] };
            }
            parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[2].Constants = { 0, 0, 24 };
            parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[3].Descriptor = { 12, 0 };
            const D3D12_ROOT_SIGNATURE_DESC desc{ 4, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
            winrt::com_ptr<ID3DBlob> serialized, errors;
            DX::ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
            DX::ThrowIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(root.put())));
            D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
            state.pRootSignature = root.get(); state.CS = { code.data(), code.size() };
            DX::ThrowIfFailed(device->CreateComputePipelineState(&state, IID_PPV_ARGS(pipeline.put())));
            const D3D12_DESCRIPTOR_HEAP_DESC heap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 44,
                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
            DX::ThrowIfFailed(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(descriptors.put())));
            retained = std::make_shared<const std::array<winrt::com_ptr<IUnknown>, 3>>(
                std::array{ root.as<IUnknown>(), pipeline.as<IUnknown>(), descriptors.as<IUnknown>() });
            failure.clear(); return true;
        } catch (const std::exception& error) {
            pipeline = nullptr; root = nullptr; descriptors = nullptr; failure = error.what(); return false;
        }
    }

    void NativeCompute::Dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* commands, uint32_t slot,
        std::span<ID3D12Resource* const> reads, std::span<ID3D12Resource* const> writes,
        std::span<const uint32_t> constants, D3D12_GPU_VIRTUAL_ADDRESS camera,
        const std::array<uint32_t, 2>& size)
    {
        CS_GPU_PASS("DX12NativeScene::NativeCompute");
        if (!pipeline || slot >= 2 || reads.size() > 14 || writes.size() > 8 || constants.size() > 24) throw std::runtime_error("Invalid native pass bindings");
        const auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = descriptors->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += uint64_t(slot * 22u) * step;
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        std::set<ID3D12Resource*> bound;
        for (uint32_t i = 0; i < 14; ++i) {
            auto* resource = i < reads.size() ? reads[i] : nullptr;
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = resource ? resource->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; desc.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(resource, &desc, cpu); cpu.ptr += step;
            if (resource && bound.insert(resource).second) barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource,
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        }
        for (uint32_t i = 0; i < 8; ++i) {
            auto* resource = i < writes.size() ? writes[i] : nullptr;
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
            desc.Format = resource ? resource->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(resource, nullptr, &desc, cpu); cpu.ptr += step;
            if (resource) barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource,
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        }
        commands->ResourceBarrier(UINT(barriers.size()), barriers.data());
        ID3D12DescriptorHeap* heaps[]{ descriptors.get() }; commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(root.get()); commands->SetPipelineState(pipeline.get());
        auto gpu = descriptors->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += uint64_t(slot * 22u) * step;
        commands->SetComputeRootDescriptorTable(0, gpu); gpu.ptr += 14u * step;
        commands->SetComputeRootDescriptorTable(1, gpu);
        std::array<uint32_t, 24> upload{};
        std::copy(constants.begin(), constants.end(), upload.begin());
        commands->SetComputeRoot32BitConstants(2, 24, upload.data(), 0);
        commands->SetComputeRootConstantBufferView(3, camera);
        commands->Dispatch((size[0] + 7u) / 8u, (size[1] + 7u) / 8u, 1);
        for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        commands->ResourceBarrier(UINT(barriers.size()), barriers.data());
    }
}

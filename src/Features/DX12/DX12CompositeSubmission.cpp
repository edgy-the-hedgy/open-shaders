#include "DX12NativeComposite.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "GpuPass.h"
#include "Utils/D3D.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <d3dcompiler.h>
#include <directx/d3dx12.h>
#include <fstream>
#include <ranges>

namespace
{
    struct CameraOptions
    {
        Matrix projectionInverse, viewInverse;
        float4 adjust;
        uint32_t width, height, count;
        float intensity;
        uint32_t sampling, phase, reconstruction, batch;
    };
    static_assert(sizeof(CameraOptions) == 176);
    struct ApplyOptions
    {
        uint32_t width, height, batches;
        int32_t selected;
        float intensity, blend;
        uint32_t preview, legacy;
    };
    static_assert(sizeof(ApplyOptions) == 32);
    bool Upload(ID3D12Device* device, winrt::com_ptr<ID3D12Resource>& target,
        const void* data, size_t size)
    {
        if (!data || !size) return false;
        if (!target || target->GetDesc().Width < size) {
            target = nullptr;
            auto desc = CD3DX12_RESOURCE_DESC::Buffer((size + 255) & ~size_t(255));
            auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(target.put())))) return false;
        }
        void* ptr = nullptr;
        if (FAILED(target->Map(0, nullptr, &ptr)) || !ptr) return false;
        std::memcpy(ptr, data, size);
        target->Unmap(0, nullptr);
        return true;
    }
}

bool DX12NativeComposite::EnsureApplyPipeline(ID3D12Device* candidate)
{
    if (applyRoot && applyPipeline && applyDevice == candidate) return true;
    if (!candidate) return false;
    try {
        std::ifstream file("Data/Shaders/DX12NativeComposite/ApplyShadowMask.dxil",
            std::ios::binary | std::ios::ate);
        if (!file || file.tellg() <= 0) return false;
        size_t size = size_t(file.tellg());
        if (size > 16u * 1024u * 1024u) return false;
        std::vector<char> shader(size);
        file.seekg(0);
        if (!file.read(shader.data(), std::streamsize(size))) return false;
        CD3DX12_DESCRIPTOR_RANGE srv, uav;
        srv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        uav.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
        std::array<CD3DX12_ROOT_PARAMETER, 3> params{};
        params[0].InitAsDescriptorTable(1, &srv);
        params[1].InitAsDescriptorTable(1, &uav);
        params[2].InitAsConstants(8, 0);
        CD3DX12_ROOT_SIGNATURE_DESC desc(UINT(params.size()), params.data());
        winrt::com_ptr<ID3DBlob> serialized, errors;
        DX::ThrowIfFailed(D3D12SerializeRootSignature(&desc,
            D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
        winrt::com_ptr<ID3D12RootSignature> root;
        DX::ThrowIfFailed(candidate->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(root.put())));
        D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
        state.pRootSignature = root.get();
        state.CS = {shader.data(), shader.size()};
        winrt::com_ptr<ID3D12PipelineState> pipeline;
        DX::ThrowIfFailed(candidate->CreateComputePipelineState(&state, IID_PPV_ARGS(pipeline.put())));
        applyRoot = std::move(root);
        applyPipeline = std::move(pipeline);
        applyDevice = candidate;
        return true;
    } catch (const std::exception&) { return false; }
}

std::shared_ptr<const DX12NativeComposite::FinalFrame>
DX12NativeComposite::CaptureAndApply(ID3D11Texture2D* raster, uint32_t frame,
    uint32_t width, uint32_t height)
{
    CS_GPU_PASS("DX12NativeComposite::CaptureAndApply");
    finishedColor.reset();
    auto& backend = globals::features::upscaling.dx12SwapChain;
    if (!raster || !IsActive() || !width || !height || !backend.d3d11Context ||
        !backend.d3d11Device || !backend.commandQueue || !backend.d3d12Device) return {};
    auto raw = globals::features::dx12NativeScene.GetRawShadow(frame);
    if (!raw || !raw->bits || !raw->raster || !raw->raster->depth ||
        !raw->readyFence || !raw->readyValue ||
        !EnsurePipelines(backend.d3d12Device.get()) ||
        !EnsureApplyPipeline(backend.d3d12Device.get())) return {};
    D3D11_TEXTURE2D_DESC source{};
    raster->GetDesc(&source);
    if (!source.Width || !source.Height || source.ArraySize != 1 || source.MipLevels != 1 ||
        source.SampleDesc.Count != 1 || width > source.Width || height > source.Height ||
        width > raw->width || height > raw->height) return {};
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = source.Format;
    if (FAILED(backend.d3d12Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
        &support, sizeof(support))) ||
        !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) ||
        !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) return {};
    auto it = std::find_if(compositeSlots.begin(), compositeSlots.end(), [](const auto& slot) {
        if (!slot || !slot->frame) return true;
        const auto done = slot->frame->producerFence ?
            slot->frame->producerFence->GetCompletedValue() : 0ull;
        if (slot->frame.use_count() != 1 || !slot->frame->producerValue ||
            done == UINT64_MAX || done < slot->frame->producerValue) return false;
        return std::ranges::all_of(slot->consumers, [](const auto& consumer) {
            const auto done = consumer.first->GetCompletedValue();
            return done != UINT64_MAX && done >= consumer.second;
        });
    });
    if (it == compositeSlots.end()) return {};
    try {
        auto& slot = *it;
        const bool recreate = !slot || !slot->input || !slot->output ||
            !slot->commands || slot->desc.Width != source.Width ||
            slot->desc.Height != source.Height || slot->desc.Format != source.Format ||
            slot->maskBatches != raw->batchCount ||
            slot->renderWidth != width || slot->renderHeight != height;
        if (recreate) {
            slot.reset();
            slot = std::make_shared<CompositeSlot>();
            slot->desc = source;
            slot->maskBatches = raw->batchCount;
            slot->renderWidth = width;
            slot->renderHeight = height;
            source.Usage = D3D11_USAGE_DEFAULT;
            source.CPUAccessFlags = source.MiscFlags = 0;
            source.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            slot->input = std::make_unique<WrappedResource>(source,
                backend.d3d11Device.get(), backend.d3d12Device.get(),
                "DX12NativeComposite::RasterInput");
            source.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            slot->output = std::make_shared<WrappedResource>(source,
                backend.d3d11Device.get(), backend.d3d12Device.get(),
                "DX12NativeComposite::FinalColor");
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_UINT,
                width, height, UINT16(raw->batchCount), 1, 1, 0,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            DX::ThrowIfFailed(backend.d3d12Device->CreateCommittedResource(&heap,
                D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(slot->filtered.put())));
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.NumDescriptors = 5;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            DX::ThrowIfFailed(backend.d3d12Device->CreateDescriptorHeap(&heapDesc,
                IID_PPV_ARGS(slot->descriptors.put())));
            DX::ThrowIfFailed(backend.d3d12Device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot->allocator.put())));
            DX::ThrowIfFailed(backend.d3d12Device->CreateCommandList(0,
                D3D12_COMMAND_LIST_TYPE_DIRECT, slot->allocator.get(),
                nullptr, IID_PPV_ARGS(slot->commands.put())));
            slot->handoff.Create(backend.d3d12Device.get(),
                backend.d3d11Device.get(), "DX12NativeComposite::FrameHandoff");
        } else {
            DX::ThrowIfFailed(slot->allocator->Reset());
            DX::ThrowIfFailed(slot->commands->Reset(slot->allocator.get(), nullptr));
        }
        constexpr size_t stride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
        CameraOptions camera{raw->raster->camera.CameraProjInverse,
            raw->raster->camera.CameraViewInverse, raw->raster->camera.CameraPosAdjust,
            width, height, raw->lightCount, 1.0f, raw->samplingMode, raw->samplingPhase,
            spatialReconstruction.load(std::memory_order_acquire) ? 1u : 0u, 0u};
        std::vector<std::array<uint8_t, stride>> constants(raw->batchCount);
        for (uint32_t batch = 0; batch < raw->batchCount; ++batch) {
            camera.batch = batch;
            std::memcpy(constants[batch].data(), &camera, sizeof(camera));
        }
        if (!Upload(backend.d3d12Device.get(), slot->camera, constants.data(),
            constants.size() * stride)) return {};
        const UINT step = backend.d3d12Device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto cpu = [&](int i) {
            return CD3DX12_CPU_DESCRIPTOR_HANDLE(slot->descriptors->GetCPUDescriptorHandleForHeapStart(), i, step);
        };
        const auto gpu = [&](int i) {
            return CD3DX12_GPU_DESCRIPTOR_HANDLE(slot->descriptors->GetGPUDescriptorHandleForHeapStart(), i, step);
        };
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        backend.d3d12Device->CreateShaderResourceView(
            raw->raster->depth->resource.get(), &srv, cpu(0));
        srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray.MipLevels = 1;
        srv.Texture2DArray.ArraySize = raw->batchCount;
        srv.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        backend.d3d12Device->CreateShaderResourceView(raw->bits.get(), &srv, cpu(1));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uav.Texture2DArray.ArraySize = raw->batchCount;
        uav.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        backend.d3d12Device->CreateUnorderedAccessView(
            slot->filtered.get(), nullptr, &uav, cpu(2));
        backend.d3d12Device->CreateShaderResourceView(
            slot->filtered.get(), &srv, cpu(3));
        uav = {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = source.Format;
        backend.d3d12Device->CreateUnorderedAccessView(
            slot->output->resource.get(), nullptr, &uav, cpu(4));

        // The reference DX11->DX12 producer fence and DX12-composite consumer.
        backend.d3d11Context->OMSetRenderTargets(0, nullptr, nullptr);
        backend.d3d11Context->CopyResource(slot->input->resource11, raster);
        const uint64_t inputReady = slot->handoff.Next();
        DX::ThrowIfFailed(backend.d3d11Context->Signal(slot->handoff.fence11.get(), inputReady));
        backend.d3d11Context->Flush();
        DX::ThrowIfFailed(backend.commandQueue->Wait(slot->handoff.fence12.get(), inputReady));
        DX::ThrowIfFailed(backend.commandQueue->Wait(raw->readyFence.get(), raw->readyValue));

        auto* cmd = slot->commands.get();
        ID3D12DescriptorHeap* heaps[]{slot->descriptors.get()};
        cmd->SetDescriptorHeaps(1, heaps);
        const std::array copyBegin{
            CD3DX12_RESOURCE_BARRIER::Transition(slot->input->resource.get(),
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(slot->output->resource.get(),
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)
        };
        cmd->ResourceBarrier(UINT(copyBegin.size()), copyBegin.data());
        cmd->CopyResource(slot->output->resource.get(), slot->input->resource.get());
        const std::array workBegin{
            CD3DX12_RESOURCE_BARRIER::Transition(slot->input->resource.get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
            CD3DX12_RESOURCE_BARRIER::Transition(slot->output->resource.get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(raw->raster->depth->resource.get(),
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(raw->bits.get(),
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        };
        cmd->ResourceBarrier(UINT(workBegin.size()), workBegin.data());
        const auto filterBegin = CD3DX12_RESOURCE_BARRIER::Transition(slot->filtered.get(),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1, &filterBegin);
        cmd->SetComputeRootSignature(reconstructionRoot.get());
        cmd->SetPipelineState(reconstructionPipeline.get());
        cmd->SetComputeRootDescriptorTable(0, gpu(0));
        cmd->SetComputeRootDescriptorTable(1, gpu(1));
        cmd->SetComputeRootDescriptorTable(2, gpu(2));
        for (uint32_t batch = 0; batch < raw->batchCount; ++batch) {
            cmd->SetComputeRootConstantBufferView(3,
                slot->camera->GetGPUVirtualAddress() + uint64_t(batch) * stride);
            cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        }
        const auto filteredReady = CD3DX12_RESOURCE_BARRIER::Transition(slot->filtered.get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &filteredReady);
        cmd->SetComputeRootSignature(applyRoot.get());
        cmd->SetPipelineState(applyPipeline.get());
        cmd->SetComputeRootDescriptorTable(0, gpu(3));
        cmd->SetComputeRootDescriptorTable(1, gpu(4));
        const int selected = diagnosticSelectedLight.load(std::memory_order_acquire);
        const ApplyOptions apply{width, height, raw->batchCount,
            selected >= 0 && uint32_t(selected) < raw->lightCount ? selected : -1,
            shadowIntensity.load(std::memory_order_acquire), 1.0f,
            diagnosticDarkenEnabled.load(std::memory_order_acquire) ? 1u : 0u,
            globals::features::linearLighting.IsLinearLightingActive() ? 0u : 1u};
        cmd->SetComputeRoot32BitConstants(2, 8, &apply, 0);
        cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        const std::array workEnd{
            CD3DX12_RESOURCE_BARRIER::Transition(slot->filtered.get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            CD3DX12_RESOURCE_BARRIER::Transition(raw->bits.get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            CD3DX12_RESOURCE_BARRIER::Transition(raw->raster->depth->resource.get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            CD3DX12_RESOURCE_BARRIER::Transition(slot->output->resource.get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)
        };
        cmd->ResourceBarrier(UINT(workEnd.size()), workEnd.data());
        DX::ThrowIfFailed(cmd->Close());
        ID3D12CommandList* lists[]{cmd};
        backend.commandQueue->ExecuteCommandLists(1, lists);
        const uint64_t done = slot->handoff.Next();
        DX::ThrowIfFailed(backend.commandQueue->Signal(slot->handoff.fence12.get(), done));
        // GPU wait only: all upscale routes see native-composited DX11 kMAIN.
        DX::ThrowIfFailed(backend.d3d11Context->Wait(slot->handoff.fence11.get(), done));
        backend.d3d11Context->CopyResource(raster, slot->output->resource11);
        auto published = std::make_shared<FinalFrame>();
        published->color = slot->output;
        published->producerFence = slot->handoff.fence12;
        published->producerValue = done;
        published->frame = frame;
        published->width = width;
        published->height = height;
        published->format = source.Format;
        slot->frame = published;
        slot->raw = std::move(raw);
        slot->consumers.clear();
        finishedColor = published;
        return published;
    } catch (const std::exception&) { return {}; }
}

bool DX12NativeComposite::RetainFinishedColor(const std::shared_ptr<const FinalFrame>& frame,
    ID3D12Fence* consumer, uint64_t value)
{
    if (!frame || !consumer || !value) return false;
    for (auto& slot : compositeSlots) {
        if (!slot || slot->frame.get() != frame.get()) continue;
        winrt::com_ptr<ID3D12Fence> held;
        held.copy_from(consumer);
        slot->consumers.emplace_back(std::move(held), value);
        return true;
    }
    return false;
}

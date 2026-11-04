#include "DirectionalTrace.h"
#include "GpuPass.h"
#include <directx/d3dx12.h>

namespace DX12Native
{
    void DirectionalTrace::CaptureRawSignals(Frame& frame, uint32_t first, uint32_t count)
    {
        CS_GPU_PASS("RTDirectLighting::CaptureRawSignals");
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        for (uint32_t i = first; i < first + count; ++i) {
            if (!frame.directSignals[i]) continue;
            auto* source = frame.directSignals[i]->resource.get();
            if (!frame.rawSignals[i]) {
                auto desc = source->GetDesc();
                desc.Flags = D3D12_RESOURCE_FLAG_NONE;
                DX::ThrowIfFailed(backend.d3d12Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                    &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(frame.rawSignals[i].put())));
                frame.rawSignals[i]->SetName(std::format(L"RTDirectLighting::RawSignal{}", i).c_str());
            }
            auto* destination = frame.rawSignals[i].get();
            std::array<D3D12_RESOURCE_BARRIER, 2> barriers{
                CD3DX12_RESOURCE_BARRIER::Transition(source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)
            };
            frame.commands->ResourceBarrier(UINT(barriers.size()), barriers.data());
            frame.commands->CopyResource(destination, source);
            for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            frame.commands->ResourceBarrier(UINT(barriers.size()), barriers.data());
        }
        if (first == 0) ++rawDirectCaptures;
        else ++rawIndirectCaptures;
    }
}

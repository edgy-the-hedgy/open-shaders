#pragma once
#include <d3d12.h>
#include <winrt/base.h>
#include <cstdint>
#include <cstring>

namespace DX12Native {
    // The owning submission retains this heap and the readback until its fence retires.
    struct GpuTimestampPair {
        winrt::com_ptr<ID3D12QueryHeap> heap;
        uint64_t frequency=0;
        bool Initialize(ID3D12Device* device,ID3D12CommandQueue* queue) {
            if(!device||!queue||FAILED(queue->GetTimestampFrequency(&frequency))||!frequency) return false;
            D3D12_QUERY_HEAP_DESC desc{};
            desc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP; desc.Count=2;
            return SUCCEEDED(device->CreateQueryHeap(&desc,IID_PPV_ARGS(heap.put())));
        }
        void Begin(ID3D12GraphicsCommandList* cmd) const {
            if(heap) cmd->EndQuery(heap.get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        }
        void End(ID3D12GraphicsCommandList* cmd,ID3D12Resource* readback,uint64_t offset) const {
            if(heap&&readback) {
                cmd->EndQuery(heap.get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
                cmd->ResolveQueryData(heap.get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,readback,offset);
            }
        }
        double Read(ID3D12Resource* readback,uint64_t offset) const {
            if(!heap||!frequency||!readback) return -1.0;
            const D3D12_RANGE range{SIZE_T(offset),SIZE_T(offset+16)};
            void* mapped=nullptr;
            if(FAILED(readback->Map(0,&range,&mapped))) return -1.0;
            uint64_t stamps[2]{};
            std::memcpy(stamps,static_cast<const char*>(mapped)+offset,sizeof(stamps));
            const D3D12_RANGE noWrite{}; readback->Unmap(0,&noWrite);
            return stamps[1]>=stamps[0]?1000.0*double(stamps[1]-stamps[0])/double(frequency):-1.0;
        }
    };
}

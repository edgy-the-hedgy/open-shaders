#include "NativeCubemaps.h"
#include "Features/DX12/DX12GpuRetirement.h"
#include "Features/DX12/DX12MaterialFrameContract.h"
#include "Utils/FileSystem.h"
#include "Utils/D3D.h"
#include "GpuPass.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <directx/d3dx12.h>
#include <fstream>
#include <stdexcept>

using namespace DX12Native;
namespace
{
    constexpr uint32_t kSize=256, kMips=9;
    constexpr DXGI_FORMAT kFormat=DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr UINT kTables=1+2*kMips;
    struct Constants {
        std::array<float4,3> dalc;
        uint32_t encoding, history, reflection, frameDelta;
    };
    static_assert(sizeof(Constants)==64);

    void Check(HRESULT hr) { DX::ThrowIfFailed(hr); }
    void Transition(ID3D12GraphicsCommandList* cmd,ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to)
    {
        const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(r,from,to);
        cmd->ResourceBarrier(1,&barrier);
    }
    void CubeSRV(ID3D12Device* device,const EnvironmentCube* cube,D3D12_CPU_DESCRIPTOR_HANDLE handle)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format=cube ? cube->format:kFormat;
        view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.TextureCube.MipLevels=cube ? cube->mipCount:1;
        device->CreateShaderResourceView(cube ? cube->resource.get():nullptr,&view,handle);
    }
}

struct NativeCubemaps::CubeProcessing::Generation
{
    EnvironmentCube history,base,reflection;
    winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
    winrt::com_ptr<ID3D12CommandAllocator> allocator;
    winrt::com_ptr<ID3D12GraphicsCommandList> commands;
    winrt::com_ptr<ID3D12RootSignature> root;
    winrt::com_ptr<ID3D12PipelineState> historyPipeline,filterPipeline;
    std::shared_ptr<const EnvironmentFrame> inputs;
    std::shared_ptr<Generation> previous;
    float4 cameraPosition{};
    uint64_t value=0, bytes=0;
};

NativeCubemaps::CubeProcessing::~CubeProcessing()
{
    current.reset(); latest.reset();
    for(auto& slot:slots)
        if(slot && !GpuFenceRetired(fence.get(),slot->value)) {
            const auto value=slot->value;
            RetireAfterFences(std::move(slot),owner.d3d12Device,{FencePoint(fence.get(),value)});
        }
}
void NativeCubemaps::CubeProcessing::Poll()
{
    for(auto& slot:slots) {
        if(!slot) continue;
        if(slot->value==UINT64_MAX && fence && owner.commandQueue && fence->GetCompletedValue()!=UINT64_MAX) {
            const auto rescue=nextFence+1;
            if(SUCCEEDED(owner.commandQueue->Signal(fence.get(),rescue))) {
                nextFence=rescue; slot->value=rescue;
            }
        }
        if(GpuFenceRetired(fence.get(),slot->value)) {
            slot->previous.reset(); slot->inputs.reset();
        }
    }
}
void NativeCubemaps::CubeProcessing::Reset()
{
    current.reset(); latest.reset(); Poll();
    for(auto& slot:slots)
        if(slot && slot.use_count()==1 && GpuFenceRetired(fence.get(),slot->value)) slot.reset();
}
void NativeCubemaps::CubeProcessing::Initialize(const GraphicsBackend& backend)
{
    if(fence) {
        if(owner.d3d12Device.get()!=backend.d3d12Device.get() || owner.commandQueue.get()!=backend.commandQueue.get())
            throw std::runtime_error("NativeCubemaps backend changed");
        return;
    }
    auto* device=backend.d3d12Device.get();
    CD3DX12_DESCRIPTOR_RANGE ranges[2];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,0);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0);
    CD3DX12_ROOT_PARAMETER params[3];
    params[0].InitAsDescriptorTable(2,ranges);
    params[1].InitAsConstants(16,0);
    params[2].InitAsConstants(4,1);
    CD3DX12_STATIC_SAMPLER_DESC samplers[2]{
        CD3DX12_STATIC_SAMPLER_DESC(0,D3D12_FILTER_MIN_MAG_MIP_POINT,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
        CD3DX12_STATIC_SAMPLER_DESC(1,D3D12_FILTER_MIN_MAG_MIP_LINEAR,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP)};
    const CD3DX12_ROOT_SIGNATURE_DESC desc(3,params,2,samplers);
    winrt::com_ptr<ID3DBlob> blob,errors;
    Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,blob.put(),errors.put()));
    winrt::com_ptr<ID3D12RootSignature> newRoot;
    Check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(newRoot.put())));
    const auto load=[&](const char* name) {
        std::ifstream file(Util::PathHelpers::GetShadersPath()/"NativeComposite"/name,std::ios::binary|std::ios::ate);
        if(!file || file.tellg()<=0 || file.tellg()>16*1024*1024) throw std::runtime_error("NativeCubemaps DXIL unavailable");
        std::vector<char> bytes(static_cast<size_t>(file.tellg()));
        file.seekg(0);
        if(!file.read(bytes.data(),std::streamsize(bytes.size()))) throw std::runtime_error("NativeCubemaps DXIL read failed");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature=newRoot.get(); pso.CS={bytes.data(),bytes.size()};
        winrt::com_ptr<ID3D12PipelineState> result;
        Check(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(result.put())));
        return result;
    };
    auto history=load("NativeCubemapHistory.dxil"),filter=load("NativeCubemapFilter.dxil");
    winrt::com_ptr<ID3D12Fence> ready;
    Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(ready.put())));
    owner=backend; root=std::move(newRoot); historyPipeline=std::move(history);
    filterPipeline=std::move(filter); fence=std::move(ready);
}
bool NativeCubemaps::CubeProcessing::Dispatch(const GraphicsBackend& backend,
    std::shared_ptr<const EnvironmentFrame> input,const std::array<float4,3>& dalc,const float4& cameraPosition)
{
    CS_GPU_PASS("NativeCubemaps::Processing");
    if(!input || !input->base || !input->base->IsReadyFor(input->stamp) ||
        !input->producerFence || !MaterialFrameContract::ValidProducerSignal(input->producerValue,input->producerFence->GetCompletedValue())) return false;
    if(latest && latest->base.stamp.SameEnvironment(input->stamp) && latest->base.revision==input->base->revision) return true;
    Initialize(backend);
    Poll();
    if(fence->GetCompletedValue()==UINT64_MAX) return false;
    auto found=std::find_if(slots.begin(),slots.end(),[&](const auto& slot) {
        return !slot || (slot.use_count()==1 && GpuFenceRetired(fence.get(),slot->value));
    });
    if(found==slots.end()) return false;
    auto* device=owner.d3d12Device.get();
    const bool reflection=!input->stamp.interior && input->reflection && input->reflection->IsReadyFor(input->stamp);
    const auto resourceDesc=[](uint32_t mips) { return CD3DX12_RESOURCE_DESC::Tex2D(kFormat,kSize,kSize,6,static_cast<UINT16>(mips),1,0,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS); };
    const auto rawDesc=resourceDesc(1), filteredDesc=resourceDesc(kMips);
    const auto rawBytes=device->GetResourceAllocationInfo(0,1,&rawDesc).SizeInBytes;
    const auto filteredBytes=device->GetResourceAllocationInfo(0,1,&filteredDesc).SizeInBytes;
    if(!rawBytes || !filteredBytes || rawBytes==UINT64_MAX || filteredBytes==UINT64_MAX) return false;
    const uint64_t required=rawBytes+filteredBytes*(reflection?2:1)+65536;
    uint64_t tracked=0;
    for(const auto& slot:slots) if(slot) tracked+=slot->bytes;
    const auto allowance=budget.Query(device,tracked);
    if(!allowance.localBudget || required>allowance.effectiveBytes || tracked>allowance.effectiveBytes-required) return false;
    auto slot=std::make_shared<Generation>();
    slot->inputs=input; slot->cameraPosition=cameraPosition; slot->bytes=required;
    slot->root=root; slot->historyPipeline=historyPipeline; slot->filterPipeline=filterPipeline;
    const auto allocate=[&](EnvironmentCube& cube,uint32_t mips) {
        const auto desc=resourceDesc(mips);
        const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(cube.resource.put())));
        cube.format=kFormat; cube.width=kSize; cube.mipCount=mips; cube.stamp=input->stamp;
        cube.revision=input->base->revision; cube.readyFence=fence;
    };
    allocate(slot->history,1); allocate(slot->base,kMips);
    if(reflection) allocate(slot->reflection,kMips);
    const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4*kTables,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(slot->descriptors.put())));
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(slot->allocator.put())));
    Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot->allocator.get(),historyPipeline.get(),IID_PPV_ARGS(slot->commands.put())));
    const uint32_t elapsed=latest ? input->stamp.engineFrame-latest->base.stamp.engineFrame:0;
    bool useHistory=false;
    if(latest && latest->base.stamp.SameEnvironment(input->stamp) && elapsed && elapsed<=120) {
        const auto& old=latest->cameraPosition;
        const float x=old.x-cameraPosition.x,y=old.y-cameraPosition.y,z=old.z-cameraPosition.z;
        useHistory=std::isfinite(x*x+y*y+z*z) && x*x+y*y+z*z<=128.f*128.f;
    }
    if(useHistory) slot->previous=latest;
    const Constants constants{dalc,uint32_t(input->stamp.encoding),uint32_t(useHistory),uint32_t(reflection),elapsed};
    auto* cmd=slot->commands.get();
    const auto step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpu=slot->descriptors->GetCPUDescriptorHandleForHeapStart();
    const auto gpu=slot->descriptors->GetGPUDescriptorHandleForHeapStart();
    ID3D12DescriptorHeap* heaps[]{slot->descriptors.get()};
    cmd->SetDescriptorHeaps(1,heaps); cmd->SetComputeRootSignature(root.get());
    cmd->SetComputeRoot32BitConstants(1,16,&constants,0);
    UINT table=0;
    const auto run=[&](const EnvironmentCube* source,const EnvironmentCube* previous,const EnvironmentCube* reflected,EnvironmentCube& destination,uint32_t mip,bool isReflection) {
        const UINT offset=4*table++;
        CubeSRV(device,source,{cpu.ptr+SIZE_T(offset)*step});
        CubeSRV(device,previous,{cpu.ptr+SIZE_T(offset+1)*step});
        CubeSRV(device,reflected,{cpu.ptr+SIZE_T(offset+2)*step});
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format=kFormat; uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uav.Texture2DArray.MipSlice=mip; uav.Texture2DArray.ArraySize=6;
        device->CreateUnorderedAccessView(destination.resource.get(),nullptr,&uav,{cpu.ptr+SIZE_T(offset+3)*step});
        const uint32_t size=std::max(1u,kSize>>mip);
        const uint32_t dispatch[]{size,mip,kMips,uint32_t(isReflection)};
        cmd->SetComputeRootDescriptorTable(0,{gpu.ptr+UINT64(offset)*step});
        cmd->SetComputeRoot32BitConstants(2,4,dispatch,0);
        cmd->Dispatch((size+7)/8,(size+7)/8,6);
    };
    const auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto write=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Transition(cmd,input->base->resource.get(),D3D12_RESOURCE_STATE_COMMON,read);
    if(useHistory) Transition(cmd,latest->history.resource.get(),D3D12_RESOURCE_STATE_COMMON,read);
    Transition(cmd,slot->history.resource.get(),D3D12_RESOURCE_STATE_COMMON,write);
    run(input->base.get(),useHistory?&latest->history:nullptr,nullptr,slot->history,0,false);
    Transition(cmd,slot->history.resource.get(),write,read);
    Transition(cmd,input->base->resource.get(),read,D3D12_RESOURCE_STATE_COMMON);
    if(useHistory) Transition(cmd,latest->history.resource.get(),read,D3D12_RESOURCE_STATE_COMMON);
    if(reflection) Transition(cmd,input->reflection->resource.get(),D3D12_RESOURCE_STATE_COMMON,read);
    cmd->SetPipelineState(filterPipeline.get());
    const auto filter=[&](EnvironmentCube& cube,bool reflected) {
        Transition(cmd,cube.resource.get(),D3D12_RESOURCE_STATE_COMMON,write);
        for(uint32_t mip=0;mip<kMips;++mip) run(&slot->history,nullptr,reflected?input->reflection.get():nullptr,cube,mip,reflected);
        Transition(cmd,cube.resource.get(),write,D3D12_RESOURCE_STATE_COMMON);
    };
    filter(slot->base,false);
    if(reflection) {
        filter(slot->reflection,true);
        Transition(cmd,input->reflection->resource.get(),read,D3D12_RESOURCE_STATE_COMMON);
    }
    Transition(cmd,slot->history.resource.get(),read,D3D12_RESOURCE_STATE_COMMON);
    Check(cmd->Close());
    Check(owner.commandQueue->Wait(input->producerFence.get(),input->producerValue));
    // Preserve every submitted owner even if signalling fails.
    slot->value=UINT64_MAX; *found=slot;
    ID3D12CommandList* lists[]{cmd}; owner.commandQueue->ExecuteCommandLists(1,lists);
    const uint64_t value=nextFence+1;
    Check(owner.commandQueue->Signal(fence.get(),value));
    nextFence=value; slot->value=value;
    slot->history.readyValue=value; slot->base.readyValue=value; slot->reflection.readyValue=value;
    auto publication=std::make_shared<EnvironmentFrame>();
    publication->stamp=input->stamp; publication->revision=value;
    publication->base=std::shared_ptr<const EnvironmentCube>(slot,&slot->base);
    if(reflection) publication->reflection=std::shared_ptr<const EnvironmentCube>(slot,&slot->reflection);
    publication->sky=input->sky;
    publication->producerFence=fence; publication->producerValue=value;
    latest=slot; current=std::move(publication);
    return true;
}

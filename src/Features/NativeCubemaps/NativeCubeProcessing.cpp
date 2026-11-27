#include "NativeCubemaps.h"
#include "NativeCubemapPolicy.h"
#include "Features/DX12/DX12GpuRetirement.h"
#include "Features/DX12/DX12MaterialFrameContract.h"
#include "Features/DX12/DX12FullNativeReconstruction.h"
#include "Utils/FileSystem.h"
#include "Utils/D3D.h"
#include "GpuPass.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <directx/d3dx12.h>
#include <stdexcept>

using namespace DX12Native;
namespace
{
    using NativeCubemaps::Policy::kSize;
    using NativeCubemaps::Policy::kMips;
    constexpr DXGI_FORMAT kFormat=DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr UINT kTables=NativeCubemaps::Policy::kMipsPerStep;
    constexpr UINT kDescriptorsPerTable=7;
    struct NativeCaptureConstants {
        Matrix view{}, projection{};
        uint32_t width=0,height=0,materialPitch=0,radiancePitch=0;
    };
    constexpr UINT kNativeCaptureDwords=36;
    static_assert(sizeof(NativeCaptureConstants)==kNativeCaptureDwords*sizeof(uint32_t));
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
    void RawSRV(ID3D12Device* device,ID3D12Resource* resource,D3D12_CPU_DESCRIPTOR_HANDLE handle)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format=DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Buffer.NumElements=resource ? UINT(resource->GetDesc().Width/4):1u;
        view.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(resource,&view,handle);
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
    std::shared_ptr<const FullNativeReconstructionFrame> nativeInput;
    std::shared_ptr<Generation> previous;
    float4 cameraPosition{};
    uint64_t value=0, bytes=0;
    NativeCubemaps::Policy::ProcessingCursor cursor{};
    Constants constants{};
    bool rasterReady=false,nativeAdmitted=false,reflectionEnabled=false;
};

NativeCubemaps::CubeProcessing::~CubeProcessing()
{
    current.reset(); latest.reset(); pending.reset();
    for(auto& slot:slots)
        if(slot && !GpuFenceRetired(fence.get(),slot->value)) {
            const auto value=slot->value;
            RetireAfterFences(std::move(slot),owner.d3d12Device,{FencePoint(fence.get(),value)});
        }
}
void NativeCubemaps::CubeProcessing::Poll()
{
    using Policy::ProcessingPhase;
    if(fence && fence->GetCompletedValue()==UINT64_MAX) {
        pending.reset(); current.reset(); latest.reset();
    }
    for(auto& slot:slots) {
        if(!slot) continue;
        if(slot->value==UINT64_MAX && fence && owner.commandQueue && fence->GetCompletedValue()!=UINT64_MAX) {
            const auto rescue=nextFence+1;
            if(SUCCEEDED(owner.commandQueue->Signal(fence.get(),rescue))) {
                nextFence=rescue; slot->value=rescue;
            }
        }
        if(slot==pending && !slot->value) continue; // initial step has not been submitted
        if(GpuFenceRetired(fence.get(),slot->value)) {
            slot->previous.reset(); slot->nativeInput.reset();
            if(slot!=pending) slot->inputs.reset();
        }
    }
    // Publish only a complete, successfully retired generation. The last
    // complete environment remains visible during every intermediate step.
    if(!pending || pending->cursor.phase!=ProcessingPhase::Complete ||
        !FenceValueCompleted(bool(fence),fence->GetCompletedValue(),pending->value)) return;
    auto slot=pending;
    const auto input=slot->inputs;
    const auto value=slot->value;
    slot->history.readyValue=value; slot->base.readyValue=value; slot->reflection.readyValue=value;
    slot->history.revision=value; slot->base.revision=value; slot->reflection.revision=value;
    auto publication=std::make_shared<EnvironmentFrame>();
    publication->stamp=input->stamp; publication->revision=value;
    publication->nativeRadianceInput=slot->nativeAdmitted;
    publication->rasterCompatibilityInput=slot->rasterReady;
    publication->base=std::shared_ptr<const EnvironmentCube>(slot,&slot->base);
    if(slot->reflectionEnabled) publication->reflection=std::shared_ptr<const EnvironmentCube>(slot,&slot->reflection);
    if(input->sky && input->sky->IsReadyFor(input->stamp) &&
        Policy::PublicationFresh(input->sky->stamp.SameEnvironment(input->stamp),input->sky->stamp.engineFrame,input->stamp.engineFrame))
        publication->sky=input->sky;
    publication->producerFence=fence; publication->producerValue=value;
    latest=slot; current=std::move(publication);
    slot->inputs.reset();
    pending.reset();
}
void NativeCubemaps::CubeProcessing::Advance(uint32_t frame)
{
    Poll();
    if(!pending || lastStepFrame==frame) return;
    if(!FenceValueCompleted(bool(fence),fence->GetCompletedValue(),pending->value)) { ++deferredSteps; return; }
    auto slot=pending;
    try {
        SubmitStep(slot,frame);
    } catch(...) {
        pending.reset();
        // Recording failure must not leave an open command list in the pool.
        // Submitted but untracked work remains retained for fence rescue.
        if(GpuFenceRetired(fence.get(),slot->value))
            for(auto& entry:slots) if(entry==slot) entry.reset();
        throw;
    }
}
json NativeCubemaps::CubeProcessing::GetDiagnostics() const
{
    uint64_t bytes=0;
    for(const auto& slot:slots) if(slot) bytes+=slot->bytes;
    return {{"allocatedBytes",bytes},{"inFlightGeneration",bool(pending)},
        {"phase",pending?uint32_t(pending->cursor.phase):uint32_t(Policy::ProcessingPhase::Complete)},
        {"nextMip",pending?pending->cursor.mip:0u},{"deferredSteps",deferredSteps}};
}
void NativeCubemaps::CubeProcessing::Reset()
{
    current.reset(); latest.reset(); pending.reset(); Poll();
    lastStepFrame=UINT32_MAX;
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
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,6,0);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0);
    CD3DX12_ROOT_PARAMETER params[4];
    params[0].InitAsDescriptorTable(2,ranges);
    params[1].InitAsConstants(16,0);
    params[2].InitAsConstants(4,1);
    params[3].InitAsConstants(kNativeCaptureDwords,2);
    CD3DX12_STATIC_SAMPLER_DESC samplers[2]{
        CD3DX12_STATIC_SAMPLER_DESC(0,D3D12_FILTER_MIN_MAG_MIP_POINT,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
        CD3DX12_STATIC_SAMPLER_DESC(1,D3D12_FILTER_MIN_MAG_MIP_LINEAR,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP)};
    const CD3DX12_ROOT_SIGNATURE_DESC desc(4,params,2,samplers);
    winrt::com_ptr<ID3DBlob> blob,errors;
    Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,blob.put(),errors.put()));
    winrt::com_ptr<ID3D12RootSignature> newRoot;
    Check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(newRoot.put())));
    const auto load=[&](const char* name) {
        const auto bytes = Util::FileHelpers::ReadBoundedBinary(Util::PathHelpers::GetShadersPath()/"NativeComposite"/name, 16 * VideoMemoryBudget::kMiB);
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
    std::shared_ptr<const EnvironmentFrame> input,const std::array<float4,3>& dalc,const float4& cameraPosition,
    std::shared_ptr<const FullNativeReconstructionFrame> native)
{
    CS_GPU_PASS("NativeCubemaps::Processing");
    using namespace MaterialFrameContract;
    if(!input || !input->stamp.scope ||
        (input->producerValue && (!input->producerFence || !ValidProducerSignal(input->producerValue,input->producerFence->GetCompletedValue())))) return false;
    const bool rasterReady=input->base && input->base->IsReadyFor(input->stamp) &&
        input->base->stamp.engineFrame==input->stamp.engineFrame;
    if(native) {
        const auto& material=native->material;
        const auto& direct=native->direct;
        if(!material || !direct || direct->material!=material || !native->ownershipMask || !native->environmentRadiance ||
            native->frame!=input->stamp.engineFrame || native->width!=material->width || native->height!=material->height ||
            material->scope!=input->stamp.scope || !material->IsCoherent(native->frame,native->width,native->height) ||
            direct->frame!=native->frame || direct->width!=native->width || direct->height!=native->height ||
            native->environmentRadiance->GetDesc().Width<uint64_t(native->width)*native->height*FullNativeReconstructionFrame::environmentRadianceStride ||
            native->ownershipMask->GetDesc().Width<uint64_t(native->width)*native->height*4 ||
            !native->readyFence || !ValidProducerSignal(native->readyValue,native->readyFence->GetCompletedValue()) ||
            !direct->readyFence || !ValidProducerSignal(direct->readyValue,direct->readyFence->GetCompletedValue()))
            native.reset();
    }
    if(!rasterReady && !native) return false;
    if(latest && latest->base.stamp.SameEnvironment(input->stamp) && latest->base.stamp.engineFrame==input->stamp.engineFrame) return true;
    Initialize(backend);
    Poll();
    if(pending || lastStepFrame==input->stamp.engineFrame || fence->GetCompletedValue()==UINT64_MAX) return false;
    auto found=std::find_if(slots.begin(),slots.end(),[&](const auto& slot) {
        return !slot || (slot.use_count()==1 && GpuFenceRetired(fence.get(),slot->value));
    });
    if(found==slots.end()) return false;
    auto* device=owner.d3d12Device.get();
    const bool reflection=!input->stamp.interior && input->reflection && input->reflection->IsReadyFor(input->stamp) &&
        Policy::PublicationFresh(input->reflection->stamp.SameEnvironment(input->stamp),
            input->reflection->stamp.engineFrame,input->stamp.engineFrame);
    const auto resourceDesc=[](uint32_t mips) { return CD3DX12_RESOURCE_DESC::Tex2D(kFormat,kSize,kSize,6,static_cast<UINT16>(mips),1,0,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS); };
    const auto rawDesc=resourceDesc(1), filteredDesc=resourceDesc(kMips);
    const auto rawBytes=device->GetResourceAllocationInfo(0,1,&rawDesc).SizeInBytes;
    const auto filteredBytes=device->GetResourceAllocationInfo(0,1,&filteredDesc).SizeInBytes;
    if(!rawBytes || !filteredBytes || rawBytes==UINT64_MAX || filteredBytes==UINT64_MAX) return false;
    const uint64_t required=rawBytes+filteredBytes*(reflection?2:1)+65536;
    // The selected slot has retired and has no external leases. Remove it
    // from the pool before recording so an exception cannot leave a partially
    // reset command list eligible for reuse.
    auto slot=std::move(*found);
    const bool reuse=slot && bool(slot->reflection.resource)==reflection;
    if(!reuse) {
        slot.reset();
        uint64_t tracked=0;
        for(const auto& other:slots) if(other) tracked+=other->bytes;
        const auto allowance=budget.Query(device,tracked);
        if(!allowance.localBudget || required>allowance.effectiveBytes || tracked>allowance.effectiveBytes-required) return false;
        slot=std::make_shared<Generation>();
        const auto allocate=[&](EnvironmentCube& cube,uint32_t mips) {
            const auto desc=resourceDesc(mips);
            const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
            Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(cube.resource.put())));
            cube.format=kFormat; cube.width=kSize; cube.mipCount=mips;
        };
        allocate(slot->history,1); allocate(slot->base,kMips);
        if(reflection) allocate(slot->reflection,kMips);
        const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,kDescriptorsPerTable*kTables,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(slot->descriptors.put())));
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(slot->allocator.put())));
        Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot->allocator.get(),historyPipeline.get(),IID_PPV_ARGS(slot->commands.put())));
        Check(slot->commands->Close());
    }
    slot->inputs=input; slot->nativeInput=native; slot->previous.reset(); slot->cameraPosition=cameraPosition; slot->bytes=required;
    slot->root=root; slot->historyPipeline=historyPipeline; slot->filterPipeline=filterPipeline;
    const auto stampCube=[&](EnvironmentCube& cube) {
        cube.stamp=input->stamp; cube.revision=nextFence+1; cube.readyFence=fence;
        cube.readyValue=0;
    };
    stampCube(slot->history); stampCube(slot->base);
    if(reflection) stampCube(slot->reflection);
    const uint32_t elapsed=latest ? input->stamp.engineFrame-latest->base.stamp.engineFrame:0;
    bool useHistory=false;
    if(latest) {
        const auto& old=latest->cameraPosition;
        const float x=old.x-cameraPosition.x,y=old.y-cameraPosition.y,z=old.z-cameraPosition.z;
        useHistory=Policy::CanReuseHistory(latest->base.stamp.SameEnvironment(input->stamp),elapsed,x*x+y*y+z*z);
    }
    if(useHistory) slot->previous=latest;
    slot->constants={dalc,uint32_t(input->stamp.encoding),uint32_t(useHistory),uint32_t(reflection),elapsed};
    slot->cursor={}; slot->rasterReady=rasterReady; slot->nativeAdmitted=bool(native); slot->reflectionEnabled=reflection;
    slot->value=0;
    *found=slot; pending=slot;
    Advance(input->stamp.engineFrame);
    return bool(pending);
}

void NativeCubemaps::CubeProcessing::SubmitStep(const std::shared_ptr<Generation>& slot, uint32_t frame)
{
    using Policy::ProcessingPhase;
    const bool history=slot->cursor.phase==ProcessingPhase::History;
    const bool reflected=slot->cursor.phase==ProcessingPhase::Reflection;
    auto* device=owner.d3d12Device.get();
    const auto& input=slot->inputs;
    const auto& native=slot->nativeInput;
    Check(slot->allocator->Reset());
    Check(slot->commands->Reset(slot->allocator.get(),history?historyPipeline.get():filterPipeline.get()));
    auto* cmd=slot->commands.get();
    const auto step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpu=slot->descriptors->GetCPUDescriptorHandleForHeapStart();
    const auto gpu=slot->descriptors->GetGPUDescriptorHandleForHeapStart();
    ID3D12DescriptorHeap* heaps[]{slot->descriptors.get()};
    cmd->SetDescriptorHeaps(1,heaps); cmd->SetComputeRootSignature(root.get());
    cmd->SetComputeRoot32BitConstants(1,16,&slot->constants,0);
    NativeCaptureConstants nativeConstants{};
    if(history && native) {
        const auto& camera=native->material->raster->camera;
        nativeConstants={camera.CameraView,camera.CameraProj,native->width,native->height,native->material->rowPitch,native->width*FullNativeReconstructionFrame::environmentRadianceStride};
    }
    cmd->SetComputeRoot32BitConstants(3,kNativeCaptureDwords,&nativeConstants,0);
    UINT table=0;
    const auto run=[&](const EnvironmentCube* source,const EnvironmentCube* previous,const EnvironmentCube* reflection,EnvironmentCube& destination,uint32_t mip,bool isReflection) {
        const UINT offset=kDescriptorsPerTable*table++;
        CubeSRV(device,source,{cpu.ptr+SIZE_T(offset)*step});
        CubeSRV(device,previous,{cpu.ptr+SIZE_T(offset+1)*step});
        CubeSRV(device,reflection,{cpu.ptr+SIZE_T(offset+2)*step});
        RawSRV(device,history && native ? native->material->samples.get():nullptr,{cpu.ptr+SIZE_T(offset+3)*step});
        RawSRV(device,history && native ? native->environmentRadiance.get():nullptr,{cpu.ptr+SIZE_T(offset+4)*step});
        RawSRV(device,history && native ? native->ownershipMask.get():nullptr,{cpu.ptr+SIZE_T(offset+5)*step});
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format=kFormat; uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uav.Texture2DArray.MipSlice=mip; uav.Texture2DArray.ArraySize=6;
        device->CreateUnorderedAccessView(destination.resource.get(),nullptr,&uav,{cpu.ptr+SIZE_T(offset+6)*step});
        const uint32_t size=std::max(1u,kSize>>mip);
        const uint32_t dispatch[]{size,mip,kMips,uint32_t(isReflection)};
        cmd->SetComputeRootDescriptorTable(0,{gpu.ptr+UINT64(offset)*step});
        cmd->SetComputeRoot32BitConstants(2,4,dispatch,0);
        cmd->Dispatch((size+7)/8,(size+7)/8,6);
    };
    const auto common=D3D12_RESOURCE_STATE_COMMON;
    const auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto write=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if(history) {
        const auto* previous=slot->previous?&slot->previous->history:nullptr;
        if(slot->rasterReady) Transition(cmd,input->base->resource.get(),common,read);
        if(previous) Transition(cmd,previous->resource.get(),common,read);
        Transition(cmd,slot->history.resource.get(),common,write);
        run(slot->rasterReady?input->base.get():nullptr,previous,nullptr,slot->history,0,false);
        Transition(cmd,slot->history.resource.get(),write,common);
        if(slot->rasterReady) Transition(cmd,input->base->resource.get(),read,common);
        if(previous) Transition(cmd,previous->resource.get(),read,common);
    } else {
        auto& cube=reflected?slot->reflection:slot->base;
        Transition(cmd,slot->history.resource.get(),common,read);
        Transition(cmd,cube.resource.get(),common,write);
        if(reflected) Transition(cmd,input->reflection->resource.get(),common,read);
        for(uint32_t mip=slot->cursor.mip;mip<slot->cursor.EndMip();++mip)
            run(&slot->history,nullptr,reflected?input->reflection.get():nullptr,cube,mip,reflected);
        if(reflected) Transition(cmd,input->reflection->resource.get(),read,common);
        Transition(cmd,cube.resource.get(),write,common);
        Transition(cmd,slot->history.resource.get(),read,common);
    }
    Check(cmd->Close());
    if(history) {
        if(input->producerValue) Check(owner.commandQueue->Wait(input->producerFence.get(),input->producerValue));
        if(native) Check(owner.commandQueue->Wait(native->readyFence.get(),native->readyValue));
    }
    slot->value=UINT64_MAX;
    ID3D12CommandList* lists[]{cmd}; owner.commandQueue->ExecuteCommandLists(1,lists);
    const uint64_t value=nextFence+1;
    Check(owner.commandQueue->Signal(fence.get(),value));
    nextFence=value; slot->value=value; lastStepFrame=frame;
    slot->cursor.Advance(slot->reflectionEnabled);
}

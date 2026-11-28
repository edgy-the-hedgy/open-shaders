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
    constexpr UINT kDescriptorsPerTable=12;
    constexpr DXGI_FORMAT kPositionFormat=DXGI_FORMAT_R32G32B32A32_FLOAT;
    struct NativeCaptureConstants {
        Matrix view{}, projection{}, inverseViewProjection{};
        uint32_t width=0,height=0,materialPitch=0,radiancePitch=0;
        float4 previousCameraDelta{};
    };
    static_assert(sizeof(NativeCaptureConstants)<=256);
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
    void TransitionCubeMip(ID3D12GraphicsCommandList* cmd,const EnvironmentCube& cube,uint32_t mip,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to)
    {
        std::array<D3D12_RESOURCE_BARRIER,6> barriers{};
        for(UINT face=0;face<6;++face)
            barriers[face]=CD3DX12_RESOURCE_BARRIER::Transition(cube.resource.get(),from,to,mip+face*cube.mipCount);
        cmd->ResourceBarrier(UINT(barriers.size()),barriers.data());
    }
    void CubeSRV(ID3D12Device* device,const EnvironmentCube* cube,D3D12_CPU_DESCRIPTOR_HANDLE handle,bool firstMipOnly=false)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format=cube ? cube->format:kFormat;
        view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.TextureCube.MipLevels=cube && !firstMipOnly ? cube->mipCount:1;
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
    EnvironmentCube history,positions,base,reflection;
    winrt::com_ptr<ID3D12Resource> captureConstants,lightingDecision;
    float ambientLuminance=0.0f, sunLuminance=0.0f;
    winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
    winrt::com_ptr<ID3D12CommandAllocator> allocator;
    winrt::com_ptr<ID3D12GraphicsCommandList> commands;
    winrt::com_ptr<ID3D12RootSignature> root;
    winrt::com_ptr<ID3D12PipelineState> historyPipeline,filterPipeline,detectPipeline;
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
    publication->defaultCube=input->defaultCube;
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
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,9,0);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0);
    CD3DX12_ROOT_PARAMETER params[4];
    params[0].InitAsDescriptorTable(2,ranges);
    params[1].InitAsConstants(16,0);
    params[2].InitAsConstants(4,1);
    params[3].InitAsConstantBufferView(2);
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
    auto history=load("NativeCubemapHistory.dxil"),filter=load("NativeCubemapFilter.dxil"),detect=load("NativeCubemapDetectLighting.dxil");
    winrt::com_ptr<ID3D12Fence> ready;
    Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(ready.put())));
    owner=backend; root=std::move(newRoot); historyPipeline=std::move(history);
    filterPipeline=std::move(filter); detectPipeline=std::move(detect); fence=std::move(ready);
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
    // A real filtered generation can be bootstrapped from engine DALC and
    // the optional default/engine-reflection cubes even without observations.
    // History receives no invented observations in this case.
    for(const auto& channel:dalc)
        if(!std::isfinite(channel.x)||!std::isfinite(channel.y)||!std::isfinite(channel.z)||!std::isfinite(channel.w)) return false;
    if(latest && latest->base.stamp.SameEnvironment(input->stamp) && latest->base.stamp.engineFrame==input->stamp.engineFrame) return true;
    Initialize(backend);
    Poll();
    if(pending || lastStepFrame==input->stamp.engineFrame || fence->GetCompletedValue()==UINT64_MAX) return false;
    auto found=std::find_if(slots.begin(),slots.end(),[&](const auto& slot) {
        return !slot || (slot.use_count()==1 && GpuFenceRetired(fence.get(),slot->value));
    });
    if(found==slots.end()) return false;
    auto* device=owner.d3d12Device.get();
    const bool reflection=!input->stamp.interior;
    const bool engineReflection=reflection && !input->stamp.skyHidden && input->reflection && input->reflection->IsReadyFor(input->stamp) &&
        Policy::PublicationFresh(input->reflection->stamp.SameEnvironment(input->stamp),
            input->reflection->stamp.engineFrame,input->stamp.engineFrame);
    const auto resourceDesc=[](uint32_t mips) { return CD3DX12_RESOURCE_DESC::Tex2D(kFormat,kSize,kSize,6,static_cast<UINT16>(mips),1,0,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS); };
    const auto rawDesc=resourceDesc(1), filteredDesc=resourceDesc(kMips);
    const auto positionDesc=CD3DX12_RESOURCE_DESC::Tex2D(kPositionFormat,kSize,kSize,6,1,1,0,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto positionBytes=device->GetResourceAllocationInfo(0,1,&positionDesc).SizeInBytes;
    const auto rawBytes=device->GetResourceAllocationInfo(0,1,&rawDesc).SizeInBytes;
    const auto filteredBytes=device->GetResourceAllocationInfo(0,1,&filteredDesc).SizeInBytes;
    if(!rawBytes || !filteredBytes || !positionBytes || rawBytes==UINT64_MAX || filteredBytes==UINT64_MAX || positionBytes==UINT64_MAX) return false;
    const uint64_t required=rawBytes+positionBytes+filteredBytes*(reflection?2:1)+3*65536;
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
        const CD3DX12_HEAP_PROPERTIES positionHeap(D3D12_HEAP_TYPE_DEFAULT);
        Check(device->CreateCommittedResource(&positionHeap,D3D12_HEAP_FLAG_NONE,&positionDesc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(slot->positions.resource.put())));
        slot->positions.format=kPositionFormat;slot->positions.width=kSize;slot->positions.mipCount=1;
        slot->captureConstants=NativeMaterialEvaluation::Buffer(device,256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        slot->lightingDecision=NativeMaterialEvaluation::Buffer(device,4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if(reflection) allocate(slot->reflection,kMips);
        const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,kDescriptorsPerTable*kTables,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(slot->descriptors.put())));
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(slot->allocator.put())));
        Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot->allocator.get(),historyPipeline.get(),IID_PPV_ARGS(slot->commands.put())));
        Check(slot->commands->Close());
    }
    slot->inputs=input; slot->nativeInput=native; slot->previous.reset(); slot->cameraPosition=cameraPosition; slot->bytes=required;
    slot->root=root; slot->historyPipeline=historyPipeline; slot->filterPipeline=filterPipeline; slot->detectPipeline=detectPipeline;
    const auto stampCube=[&](EnvironmentCube& cube) {
        cube.stamp=input->stamp; cube.revision=nextFence+1; cube.readyFence=fence;
        cube.readyValue=0;
    };
    stampCube(slot->history); stampCube(slot->positions); stampCube(slot->base);
    if(reflection) stampCube(slot->reflection);
    const uint32_t elapsed=latest ? input->stamp.engineFrame-latest->base.stamp.engineFrame:0;
    const auto luminance=[](float r,float g,float b){return 0.2126f*r+0.7152f*g+0.0722f*b;};
    // x is the constant SH coefficient. Keep native sunlight separate so a
    // deferred native frame cannot masquerade as an actual lighting step.
    slot->ambientLuminance=std::max(0.0f,luminance(dalc[0].x,dalc[1].x,dalc[2].x));
    const auto sun=native?native->direct->directional.radiance:float4{};
    slot->sunLuminance=std::max(0.0f,luminance(sun.x,sun.y,sun.z));
    bool useHistory=false;
    if(latest) {
        const auto& old=latest->cameraPosition;
        const float x=old.x-cameraPosition.x,y=old.y-cameraPosition.y,z=old.z-cameraPosition.z;
        const bool lightChanged=Policy::LightingDiscontinuity(latest->ambientLuminance,slot->ambientLuminance) ||
            (latest->nativeAdmitted && native && Policy::LightingDiscontinuity(latest->sunLuminance,slot->sunLuminance));
        useHistory=Policy::CanReuseHistory(latest->base.stamp.SameEnvironment(input->stamp),elapsed,x*x+y*y+z*z) && !lightChanged;
    }
    if(useHistory) slot->previous=latest;
    slot->constants={dalc,uint32_t(input->stamp.encoding),uint32_t(useHistory),uint32_t(engineReflection) | (!input->stamp.skyHidden ? 2u : 0u) | (input->defaultCube && input->defaultCube->IsReadyFor(input->stamp) ? 4u : 0u),elapsed};
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
        nativeConstants={camera.CameraView,camera.CameraProj,camera.CameraViewProjInverse,native->width,native->height,native->material->rowPitch,native->width*FullNativeReconstructionFrame::environmentRadianceStride};
    }
    if(history && slot->previous) {
        const auto& old=slot->previous->cameraPosition; const auto& now=slot->cameraPosition;
        nativeConstants.previousCameraDelta={old.x-now.x,old.y-now.y,old.z-now.z,0};
    }
    void* mapped=nullptr;const D3D12_RANGE noRead{};
    Check(slot->captureConstants->Map(0,&noRead,&mapped));
    std::memcpy(mapped,&nativeConstants,sizeof(nativeConstants));slot->captureConstants->Unmap(0,nullptr);
    cmd->SetComputeRootConstantBufferView(3,slot->captureConstants->GetGPUVirtualAddress());
    UINT table=0;
    const auto run=[&](const EnvironmentCube* source,const EnvironmentCube* previous,const EnvironmentCube* reflection,EnvironmentCube& destination,uint32_t mip,bool isReflection) {
        const UINT offset=kDescriptorsPerTable*table++;
        CubeSRV(device,source,{cpu.ptr+SIZE_T(offset)*step},!history && mip!=0);
        CubeSRV(device,previous,{cpu.ptr+SIZE_T(offset+1)*step});
        CubeSRV(device,reflection,{cpu.ptr+SIZE_T(offset+2)*step});
        RawSRV(device,history && native ? native->material->samples.get():nullptr,{cpu.ptr+SIZE_T(offset+3)*step});
        RawSRV(device,history && native ? native->environmentRadiance.get():nullptr,{cpu.ptr+SIZE_T(offset+4)*step});
        RawSRV(device,history && native ? native->ownershipMask.get():nullptr,{cpu.ptr+SIZE_T(offset+5)*step});
        CubeSRV(device,!history && mip==0 && (slot->constants.reflection&4u)!=0u?input->defaultCube.get():nullptr,{cpu.ptr+SIZE_T(offset+6)*step});
        CubeSRV(device,history && slot->previous?&slot->previous->positions:nullptr,{cpu.ptr+SIZE_T(offset+7)*step});
        D3D12_SHADER_RESOURCE_VIEW_DESC depthView{};depthView.Format=DXGI_FORMAT_R32_FLOAT;depthView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;depthView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;depthView.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(history && native?native->material->raster->depth->resource.get():nullptr,&depthView,{cpu.ptr+SIZE_T(offset+8)*step});
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format=kFormat; uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uav.Texture2DArray.MipSlice=mip; uav.Texture2DArray.ArraySize=6;
        device->CreateUnorderedAccessView(destination.resource.get(),nullptr,&uav,{cpu.ptr+SIZE_T(offset+9)*step});
        uav.Format=kPositionFormat;uav.Texture2DArray.MipSlice=0;
        device->CreateUnorderedAccessView(history?slot->positions.resource.get():nullptr,nullptr,&uav,{cpu.ptr+SIZE_T(offset+10)*step});
        D3D12_UNORDERED_ACCESS_VIEW_DESC decision{};
        decision.Format=DXGI_FORMAT_R32_TYPELESS;decision.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
        decision.Buffer.NumElements=1;decision.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(history?slot->lightingDecision.get():nullptr,nullptr,&decision,{cpu.ptr+SIZE_T(offset+11)*step});
        const uint32_t size=std::max(1u,kSize>>mip);
        const uint32_t dispatch[]{size,mip,kMips,uint32_t(isReflection)};
        cmd->SetComputeRootDescriptorTable(0,{gpu.ptr+UINT64(offset)*step});
        cmd->SetComputeRoot32BitConstants(2,4,dispatch,0);
        if(history){
            cmd->SetPipelineState(detectPipeline.get());
            cmd->Dispatch(1,1,1);
            const auto barrier=CD3DX12_RESOURCE_BARRIER::UAV(slot->lightingDecision.get());
            cmd->ResourceBarrier(1,&barrier);
            cmd->SetPipelineState(historyPipeline.get());
        }
        cmd->Dispatch((size+7)/8,(size+7)/8,6);
    };
    const auto common=D3D12_RESOURCE_STATE_COMMON;
    const auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto write=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if(history) {
        const auto* previous=slot->previous?&slot->previous->history:nullptr;
        if(slot->rasterReady) Transition(cmd,input->base->resource.get(),common,read);
        if(previous) {Transition(cmd,previous->resource.get(),common,read);Transition(cmd,slot->previous->positions.resource.get(),common,read);}
        if(native) Transition(cmd,native->material->raster->depth->resource.get(),common,read);
        Transition(cmd,slot->positions.resource.get(),common,write);
        Transition(cmd,slot->history.resource.get(),common,write);
        Transition(cmd,slot->lightingDecision.get(),common,write);
        run(slot->rasterReady?input->base.get():nullptr,previous,nullptr,slot->history,0,false);
        Transition(cmd,slot->lightingDecision.get(),write,common);
        Transition(cmd,slot->history.resource.get(),write,common);
        if(slot->rasterReady) Transition(cmd,input->base->resource.get(),read,common);
        if(previous) {Transition(cmd,previous->resource.get(),read,common);Transition(cmd,slot->previous->positions.resource.get(),read,common);}
        if(native) Transition(cmd,native->material->raster->depth->resource.get(),read,common);
        Transition(cmd,slot->positions.resource.get(),write,common);
    } else {
        auto& cube=reflected?slot->reflection:slot->base;
        const bool inference=slot->cursor.mip==0;
        const bool engineReflection=inference && reflected && (slot->constants.reflection&1u)!=0u;
        if(inference) Transition(cmd,slot->history.resource.get(),common,read);
        else TransitionCubeMip(cmd,cube,0,common,read);
        if(engineReflection) Transition(cmd,input->reflection->resource.get(),common,read);
        const bool useDefault=inference && (slot->constants.reflection&4u)!=0u;
        if(useDefault) Transition(cmd,input->defaultCube->resource.get(),common,read);
        for(uint32_t mip=slot->cursor.mip;mip<slot->cursor.EndMip();++mip) {
            TransitionCubeMip(cmd,cube,mip,common,write);
            run(inference?&slot->history:&cube,nullptr,engineReflection?input->reflection.get():nullptr,cube,mip,reflected);
            TransitionCubeMip(cmd,cube,mip,write,common);
        }
        if(engineReflection) Transition(cmd,input->reflection->resource.get(),read,common);
        if(useDefault) Transition(cmd,input->defaultCube->resource.get(),read,common);
        if(inference) Transition(cmd,slot->history.resource.get(),read,common);
        else TransitionCubeMip(cmd,cube,0,read,common);
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

#include "NativeForwardEnvironment.h"
#include "NativeCubemapPolicy.h"
#include "Features/DX12/DX12GpuRetirement.h"
#include "Features/DX12/DX12MaterialFrameContract.h"
#include "Utils/D3D.h"
#include "GpuPass.h"
#include <algorithm>
#include <directx/d3dx12.h>
#include <stdexcept>

using namespace DX12Native;
namespace
{
    constexpr UINT kReflectionSlot=30;
    constexpr UINT kFaces=6;
    constexpr UINT kMaxMips=15;
    struct CubeCopy
    {
        std::unique_ptr<WrappedResource> strip;
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11ShaderResourceView> view;
        UINT width=0,mips=0;
        DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    };
    UINT StripHeight(UINT width,UINT mips)
    {
        UINT height=0;for(UINT m=0;m<mips;++m)height+=std::max(1u,width>>m);
        return height;
    }
    bool ValidCube(const EnvironmentCube& cube,const EnvironmentStamp& requested)
    {
        return cube.IsReadyFor(requested)&&NativeCubemaps::Policy::PublicationFresh(true,cube.stamp.engineFrame,requested.engineFrame)&&
            cube.format==DXGI_FORMAT_R16G16B16A16_FLOAT&&cube.width<=D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION/kFaces&&cube.mipCount<=kMaxMips;
    }
}
struct NativeCubemaps::ForwardEnvironment::Slot
{
    std::array<CubeCopy,2> cubes;
    std::shared_ptr<const EnvironmentFrame> source;
    EnvironmentStamp stamp{};
    uint64_t revision=0,ready=0,read=0,bytes=0;
    winrt::com_ptr<ID3D12CommandAllocator> allocator;
    winrt::com_ptr<ID3D12GraphicsCommandList> commands;
};
NativeCubemaps::ForwardEnvironment::~ForwardEnvironment()
{
    Reset();
    for(auto& slot:slots)if(slot){
        std::vector<FenceDependency> deps{FencePoint(produced.fence12.get(),slot->ready),FencePoint(consumed.fence12.get(),slot->read)};
        RetireAfterFences(std::move(slot),owner.d3d12Device,std::move(deps));
    }
}
void NativeCubemaps::ForwardEnvironment::Poll()
{
    for(auto& slot:slots)if(slot&&GpuFenceRetired(produced.fence12.get(),slot->ready))slot->source.reset();
}
void NativeCubemaps::ForwardEnvironment::Reset()
{
    Unbind();current.reset();pending.reset();Poll();
}
void NativeCubemaps::ForwardEnvironment::Update(const GraphicsBackend& backend,std::shared_ptr<const EnvironmentFrame> source,const EnvironmentStamp& requested)
{
    if(bound) return;
    CS_GPU_PASS("NativeCubemaps::ForwardPublication");
    try{
        if(!produced.fence12){
            SharedFence p,c;
            p.Create(backend.d3d12Device.get(),backend.d3d11Device.get(),"NativeCubemaps::ForwardProduced");
            c.Create(backend.d3d12Device.get(),backend.d3d11Device.get(),"NativeCubemaps::ForwardConsumed");
            owner=backend;produced=std::move(p);consumed=std::move(c);
        }
        if(owner.d3d12Device.get()!=backend.d3d12Device.get()||owner.commandQueue.get()!=backend.commandQueue.get()||
           owner.d3d11Context.get()!=backend.d3d11Context.get())throw std::runtime_error("Forward environment backend changed");
        Poll();
        if(current&&!Policy::PublicationFresh(current->stamp.SameEnvironment(requested),current->stamp.engineFrame,requested.engineFrame))current.reset();
        if(pending&& !pending->stamp.SameEnvironment(requested))pending.reset();
        if(pending){
            if(!GpuFenceRetired(produced.fence12.get(),pending->ready)){++deferred;return;}
            if(!Policy::PublicationFresh(true,pending->stamp.engineFrame,requested.engineFrame)){pending.reset();return;}
            auto* context=owner.d3d11Context.get();
            DX::ThrowIfFailed(context->Wait(produced.fence11.get(),pending->ready));
            pending->read=UINT64_MAX;
            for(const auto& cube:pending->cubes)if(cube.texture){
                UINT y=0;
                for(UINT mip=0;mip<cube.mips;++mip){
                    const UINT size=std::max(1u,cube.width>>mip);
                    for(UINT face=0;face<kFaces;++face){
                        const D3D11_BOX box{face*size,y,0,(face+1)*size,y+size,1};
                        context->CopySubresourceRegion(cube.texture.get(),D3D11CalcSubresource(mip,face,cube.mips),0,0,0,cube.strip->resource11,0,&box);
                    }
                    y+=size;
                }
            }
            const uint64_t value=consumed.Next();
            DX::ThrowIfFailed(context->Signal(consumed.fence11.get(),value));pending->read=value;
            context->Flush();current=std::move(pending);++publications;failure.clear();
        }
        if(!source||!source->base||!source->stamp.SameEnvironment(requested)||!ValidCube(*source->base,requested)||
           !source->producerFence||!MaterialFrameContract::ValidProducerSignal(source->producerValue,source->producerFence->GetCompletedValue()))return;
        if(current&&current->revision==source->revision&&current->stamp.SameEnvironment(requested))return;
        // Do not insert a same-frame wait for unfinished environment inference.
        if(!GpuFenceRetired(source->producerFence.get(),source->producerValue)){++deferred;return;}
        auto selected=std::find_if(slots.begin(),slots.end(),[&](const auto& slot){
            return !slot||(slot.use_count()==1&&GpuFenceRetired(produced.fence12.get(),slot->ready)&&GpuFenceRetired(consumed.fence12.get(),slot->read));
        });
        if(selected==slots.end()){++deferred;return;}
        auto* device=owner.d3d12Device.get();
        const std::array<std::shared_ptr<const EnvironmentCube>,2> sources{
            source->base,source->reflection&&ValidCube(*source->reflection,requested)?source->reflection:nullptr};
        uint64_t bytes=0;
        for(const auto& cube:sources)if(cube){
            const auto strip=CD3DX12_RESOURCE_DESC::Tex2D(cube->format,kFaces*cube->width,StripHeight(cube->width,cube->mipCount));
            const auto full=CD3DX12_RESOURCE_DESC::Tex2D(cube->format,cube->width,cube->width,kFaces,UINT16(cube->mipCount));
            const auto a=device->GetResourceAllocationInfo(0,1,&strip).SizeInBytes;
            const auto b=device->GetResourceAllocationInfo(0,1,&full).SizeInBytes;
            if(a==UINT64_MAX||b==UINT64_MAX||a>UINT64_MAX-b||bytes>UINT64_MAX-a-b)throw std::runtime_error("Forward cube size invalid");
            bytes+=a+b;
        }
        selected->reset();
        uint64_t tracked=0;for(const auto& slot:slots)if(slot)tracked+=slot->bytes;
        const auto allowance=budget.Query(device,tracked).effectiveBytes;
        if(bytes>allowance||tracked>allowance-bytes){++deferred;return;}
        auto next=std::make_shared<Slot>();next->bytes=bytes;next->stamp=source->stamp;next->revision=source->revision;next->source=source;
        DX::ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(next->allocator.put())));
        DX::ThrowIfFailed(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,next->allocator.get(),nullptr,IID_PPV_ARGS(next->commands.put())));
        auto* cmd=next->commands.get();
        for(size_t i=0;i<sources.size();++i){
            const auto& src=sources[i];if(!src)continue;
            auto& cube=next->cubes[i];cube.width=src->width;cube.mips=src->mipCount;cube.format=src->format;
            D3D11_TEXTURE2D_DESC desc{};desc.Width=kFaces*cube.width;desc.Height=StripHeight(cube.width,cube.mips);
            desc.ArraySize=1;desc.MipLevels=1;desc.Format=cube.format;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            cube.strip=std::make_unique<WrappedResource>(desc,owner.d3d11Device.get(),device,"NativeCubemaps::ForwardStrip");
            desc.Width=desc.Height=cube.width;desc.ArraySize=kFaces;desc.MipLevels=cube.mips;desc.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;
            DX::ThrowIfFailed(owner.d3d11Device->CreateTexture2D(&desc,nullptr,cube.texture.put()));
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=cube.format;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURECUBE;sd.TextureCube.MipLevels=cube.mips;
            DX::ThrowIfFailed(owner.d3d11Device->CreateShaderResourceView(cube.texture.get(),&sd,cube.view.put()));
            auto* packed=cube.strip->resource.get();auto* original=src->resource.get();
            const D3D12_RESOURCE_BARRIER begin[]{CD3DX12_RESOURCE_BARRIER::Transition(original,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(packed,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST)};
            cmd->ResourceBarrier(2,begin);
            UINT y=0;
            for(UINT mip=0;mip<cube.mips;++mip){
                const UINT size=std::max(1u,cube.width>>mip);
                for(UINT face=0;face<kFaces;++face){
                    const CD3DX12_TEXTURE_COPY_LOCATION from(original,D3D11CalcSubresource(mip,face,cube.mips)),to(packed,0);
                    cmd->CopyTextureRegion(&to,face*size,y,0,&from,nullptr);
                }
                y+=size;
            }
            const D3D12_RESOURCE_BARRIER end[]{CD3DX12_RESOURCE_BARRIER::Transition(original,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON),
                CD3DX12_RESOURCE_BARRIER::Transition(packed,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON)};
            cmd->ResourceBarrier(2,end);
        }
        DX::ThrowIfFailed(cmd->Close());
        *selected=next;
        next->ready=UINT64_MAX;
        ID3D12CommandList* lists[]{cmd};owner.commandQueue->ExecuteCommandLists(1,lists);
        const uint64_t value=produced.Next();DX::ThrowIfFailed(owner.commandQueue->Signal(produced.fence12.get(),value));
        next->ready=value;pending=std::move(next);
    }catch(const std::exception& e){
        if(failure!=e.what())logger::warn("[NativeCubemaps] Forward environment: {}",e.what());
        failure=e.what();pending.reset();
    }
}
bool NativeCubemaps::ForwardEnvironment::Bind(const GraphicsBackend& backend,const EnvironmentStamp& requested)
{
    if(bound) Unbind();
    if(!backend.d3d11Context || !current || !current->cubes[0].view ||
       !Policy::PublicationFresh(current->stamp.SameEnvironment(requested),current->stamp.engineFrame,requested.engineFrame)) return false;
    boundContext=backend.d3d11Context;
    ID3D11ShaderResourceView* saved[2]{};boundContext->PSGetShaderResources(kReflectionSlot,2,saved);
    for(size_t i=0;i<prior.size();++i)prior[i].attach(saved[i]);
    if(current&&Policy::PublicationFresh(current->stamp.SameEnvironment(requested),current->stamp.engineFrame,requested.engineFrame)){
        boundViews[1]=current->cubes[0].view;
        boundViews[0]=current->cubes[1].view?current->cubes[1].view:boundViews[1];
    }
    ID3D11ShaderResourceView* views[]{boundViews[0].get(),boundViews[1].get()};
    boundContext->PSSetShaderResources(kReflectionSlot,2,views);bound=true;
    ID3D11ShaderResourceView* actual[2]{};boundContext->PSGetShaderResources(kReflectionSlot,2,actual);
    const bool ready=actual[0]==views[0]&&actual[1]==views[1]&&actual[0]&&actual[1];
    for(auto* view:actual)if(view)view->Release();
    if(!ready)Unbind();
    return ready;
}
void NativeCubemaps::ForwardEnvironment::Unbind()
{
    if(!bound)return;
    ID3D11ShaderResourceView* views[2]{};boundContext->PSGetShaderResources(kReflectionSlot,2,views);
    for(UINT i=0;i<2;++i){
        if(views[i]==boundViews[i].get()){auto* restore=prior[i].get();boundContext->PSSetShaderResources(kReflectionSlot+i,1,&restore);}
        if(views[i])views[i]->Release();
    }
    // Retire after the last draw, not merely after the strip-to-cube copy.
    if(current && consumed.fence11){
        current->read=UINT64_MAX;
        const uint64_t value=consumed.Next();
        if(SUCCEEDED(boundContext->Signal(consumed.fence11.get(),value)))current->read=value;
    }
    prior={};boundViews={};boundContext=nullptr;bound=false;
}
json NativeCubemaps::ForwardEnvironment::Diagnostics() const
{
    uint64_t bytes=0;for(const auto& slot:slots)if(slot)bytes+=slot->bytes;
    return {{"revision",current?current->revision:0u},{"published",bool(current)},{"pending",bool(pending)},
        {"publications",publications},{"deferred",deferred},{"allocatedBytes",bytes},{"failure",failure}};
}

#include "NativeWaterSSR.h"
#include "NativeCubemapPolicy.h"
#include "Features/DX12/DX12GpuRetirement.h"
#include "Globals.h"
#include "GpuPass.h"
#include "Utils/D3D.h"
#include "Utils/FileSystem.h"
#include <algorithm>
#include <cmath>
#include <directx/d3dx12.h>
#include <stdexcept>

using namespace DX12Native;
namespace
{
    constexpr UINT kReturnSlot=5;
    constexpr UINT kGroup=8;
    struct CaptureConstants { uint32_t width,height; float scaleX,scaleY; };
    struct Constants {
        Matrix projection,inverseProjection,inverseView;
        uint32_t width,height,outputWidth,outputHeight;
        uint32_t reverseZ,sky,encoding,gammaInput;
        uint32_t performance,padding[3]{};
    };
    static_assert(sizeof(Constants)==240);
    constexpr UINT kConstants=sizeof(Constants)/sizeof(uint32_t);
    void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        const auto barrier=CD3DX12_RESOURCE_BARRIER::Transition(resource,from,to);
        cmd->ResourceBarrier(1,&barrier);
    }
    bool TextureInput(ID3D11ShaderResourceView* view)
    {
        if (!view) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        view->GetDesc(&desc);
        if (desc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D) return false;
        winrt::com_ptr<ID3D11Resource> resource;
        view->GetResource(resource.put());
        auto texture=resource.try_as<ID3D11Texture2D>();
        if (!texture) return false;
        D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
        return td.Width && td.Height && td.ArraySize==1 && td.SampleDesc.Count==1;
    }
}

struct NativeCubemaps::WaterSSR::Slot
{
    std::unique_ptr<WrappedResource> raw,mask,result;
    winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
    winrt::com_ptr<ID3D12CommandAllocator> allocator;
    winrt::com_ptr<ID3D12GraphicsCommandList> commands;
    winrt::com_ptr<ID3D12RootSignature> root;
    winrt::com_ptr<ID3D12PipelineState> pipeline;
    std::shared_ptr<const EnvironmentFrame> environment;
    uint32_t width=0,height=0,outWidth=0,outHeight=0;
    uint64_t input=0,ready=0,consumed=0,bytes=0;
};

NativeCubemaps::WaterSSR::~WaterSSR()
{
    End();
    for(auto& slot:slots) if(slot) {
        std::vector<FenceDependency> dependencies{FencePoint(inputFence.fence12.get(),slot->input),
            FencePoint(outputFence.fence12.get(),slot->ready),FencePoint(consumedFence.fence12.get(),slot->consumed)};
        RetireAfterFences(std::move(slot),owner.d3d12Device,std::move(dependencies));
    }
}

void NativeCubemaps::WaterSSR::Initialize(const GraphicsBackend& backend)
{
    if (pipeline) {
        if(owner.d3d12Device.get()!=backend.d3d12Device.get() || owner.commandQueue.get()!=backend.commandQueue.get())
            throw std::runtime_error("Native water SSR backend changed");
        return;
    }
    auto* device=backend.d3d12Device.get();
    CD3DX12_DESCRIPTOR_RANGE ranges[2];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,0);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0);
    CD3DX12_ROOT_PARAMETER params[2];
    params[0].InitAsDescriptorTable(2,ranges);
    params[1].InitAsConstants(kConstants,0);
    const CD3DX12_STATIC_SAMPLER_DESC sampler(0,D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    const CD3DX12_ROOT_SIGNATURE_DESC signature(2,params,1,&sampler);
    winrt::com_ptr<ID3DBlob> blob,errors;
    winrt::com_ptr<ID3D12RootSignature> nextRoot;
    winrt::com_ptr<ID3D12PipelineState> nextPipeline;
    DX::ThrowIfFailed(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,blob.put(),errors.put()));
    DX::ThrowIfFailed(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(nextRoot.put())));
    const auto base=Util::PathHelpers::GetShadersPath();
    const auto shader=Util::FileHelpers::ReadBoundedBinary(base/"NativeComposite"/"NativeWaterSSR.dxil",16*VideoMemoryBudget::kMiB);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature=nextRoot.get();pso.CS={shader.data(),shader.size()};
    DX::ThrowIfFailed(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(nextPipeline.put())));
    winrt::com_ptr<ID3D11ComputeShader> nextCapture;
    winrt::com_ptr<ID3D11PixelShader> nextReturn;
    nextCapture.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader((base/"NativeCubemaps"/"CaptureWaterSSR.cs.hlsl").c_str(),{},"cs_5_0")));
    nextReturn.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader((base/"NativeCubemaps"/"ReturnWaterSSR.ps.hlsl").c_str(),{},"ps_5_0")));
    if (!nextCapture || !nextReturn) throw std::runtime_error("Native water SSR interop shaders unavailable");
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(CaptureConstants);cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    winrt::com_ptr<ID3D11Buffer> nextCB;
    DX::ThrowIfFailed(backend.d3d11Device->CreateBuffer(&cb,nullptr,nextCB.put()));
    SharedFence in,out,done;
    in.Create(device,backend.d3d11Device.get(),"NativeWaterSSR::Inputs");
    out.Create(device,backend.d3d11Device.get(),"NativeWaterSSR::Result");
    done.Create(device,backend.d3d11Device.get(),"NativeWaterSSR::Consumed");
    owner=backend;root=std::move(nextRoot);pipeline=std::move(nextPipeline);
    capture=std::move(nextCapture);returnShader=std::move(nextReturn);captureCB=std::move(nextCB);
    inputFence=std::move(in);outputFence=std::move(out);consumedFence=std::move(done);
}

void NativeCubemaps::WaterSSR::Poll()
{
    for(auto& slot:slots) if(slot && GpuFenceRetired(outputFence.fence12.get(),slot->ready)) slot->environment.reset();
}

void NativeCubemaps::WaterSSR::Draw(const GraphicsBackend& backend,const NativeWorldFrame& world,std::shared_ptr<const EnvironmentFrame> environment,
    EnvironmentEncoding encoding,bool gammaInput,bool skyVisible,bool performance,ID3D11PixelShader* expected)
{
    if (active || !expected || world.width<2 || !world.height || !globals::d3d::context) return;
    CS_GPU_PASS("NativeCubemaps::WaterSSR");
    auto* context=backend.d3d11Context.get();
    if(!context) return;
    winrt::com_ptr<ID3D11PixelShader> boundShader;
    context->PSGetShader(boundShader.put(),nullptr,nullptr);
    if (boundShader.get()!=expected) {++rejected;return;}
    D3D11_VIEWPORT viewport{};UINT count=1;context->RSGetViewports(&count,&viewport);
    if(count!=1 || viewport.TopLeftX!=0 || viewport.TopLeftY!=0 ||
        !std::isfinite(viewport.Width) || !std::isfinite(viewport.Height) ||
        viewport.Width<1 || viewport.Height<1 || viewport.Width>world.displayWidth || viewport.Height>world.displayHeight ||
        std::floor(viewport.Width)!=viewport.Width || std::floor(viewport.Height)!=viewport.Height) {++rejected;return;}
    const uint32_t ow=uint32_t(viewport.Width),oh=uint32_t(viewport.Height);
    std::array<winrt::com_ptr<ID3D11ShaderResourceView>,3> input;
    ID3D11ShaderResourceView* reads[3]{};context->PSGetShaderResources(0,3,reads);
    for(size_t i=0;i<input.size();++i) input[i].attach(reads[i]);
    winrt::com_ptr<ID3D11Buffer> geometry;
    UINT firstConstant=0,numConstants=0;
    context->PSGetConstantBuffers1(2,1,geometry.put(),&firstConstant,&numConstants);
    D3D11_BUFFER_DESC gd{};if(geometry)geometry->GetDesc(&gd);
    if(!geometry || numConstants<2 || uint64_t(firstConstant)*16+32>gd.ByteWidth || !std::ranges::all_of(input,[](const auto& v){return TextureInput(v.get());})) {++rejected;return;}
    winrt::com_ptr<ID3D11RenderTargetView> target;
    context->OMGetRenderTargets(1,target.put(),nullptr);
    if(!target){++rejected;return;}
    winrt::com_ptr<ID3D11Resource> targetResource;target->GetResource(targetResource.put());
    auto targetTexture=targetResource.try_as<ID3D11Texture2D>();
    D3D11_TEXTURE2D_DESC td{};if(targetTexture)targetTexture->GetDesc(&td);
    if(!targetTexture || td.ArraySize!=1 || td.SampleDesc.Count!=1 || td.Width<ow || td.Height<oh){++rejected;return;}
    const auto scale=world.camera.DynamicResolutionParams1;
    if(!std::isfinite(scale.x)||!std::isfinite(scale.y)||scale.x<=0||scale.y<=0||scale.x>1||scale.y>1){++rejected;return;}
    std::shared_ptr<Slot>* selected=nullptr;
    try {
        Initialize(backend);
        Poll();
        auto* device=owner.d3d12Device.get();
        if(outputFence.fence12->GetCompletedValue()==UINT64_MAX) throw std::runtime_error("Water SSR device removed");
        for(auto& slot:slots) if(!slot || (GpuFenceRetired(inputFence.fence12.get(),slot->input)&&
            GpuFenceRetired(outputFence.fence12.get(),slot->ready)&&GpuFenceRetired(consumedFence.fence12.get(),slot->consumed))) {
            selected=&slot;
            if(slot && slot->width==world.width&&slot->height==world.height&&slot->outWidth==ow&&slot->outHeight==oh) break;
        }
        if(!selected){++rejected;return;}
        auto& slot=*selected;
        if(!slot||slot->width!=world.width||slot->height!=world.height||slot->outWidth!=ow||slot->outHeight!=oh){
            const auto alloc=[&](DXGI_FORMAT fmt,uint32_t w,uint32_t h){
                const auto desc=CD3DX12_RESOURCE_DESC::Tex2D(fmt,w,h,1,1,1,0,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                return device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
            };
            const auto a=alloc(DXGI_FORMAT_R32G32B32A32_FLOAT,world.width,world.height+1);
            const auto b=alloc(DXGI_FORMAT_R32_FLOAT,world.width,world.height);
            const auto c=alloc(DXGI_FORMAT_R16G16B16A16_FLOAT,ow,oh);
            if(a==UINT64_MAX||b==UINT64_MAX||c==UINT64_MAX||a>UINT64_MAX-b||a+b>UINT64_MAX-c)throw std::runtime_error("Water SSR allocation unsupported");
            uint64_t tracked=0;for(const auto& old:slots)if(old)tracked+=old->bytes;
            const auto allowance=budget.Query(device,tracked).effectiveBytes;
            if(a+b+c>allowance||tracked>allowance-(a+b+c))throw std::runtime_error("Water SSR deferred by DXGI budget");
            auto next=std::make_shared<Slot>();
            auto texture=[&](DXGI_FORMAT fmt,uint32_t w,uint32_t h,const char* name){
                D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.ArraySize=1;desc.Format=fmt;
                desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
                return std::make_unique<WrappedResource>(desc,owner.d3d11Device.get(),device,name);
            };
            next->raw=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,world.width,world.height+1,"NativeWaterSSR::Raw");
            next->mask=texture(DXGI_FORMAT_R32_FLOAT,world.width,world.height,"NativeWaterSSR::Mask");
            next->result=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,ow,oh,"NativeWaterSSR::Result");
            next->width=world.width;next->height=world.height;next->outWidth=ow;next->outHeight=oh;next->bytes=a+b+c;
            const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            DX::ThrowIfFailed(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(next->descriptors.put())));
            DX::ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(next->allocator.put())));
            DX::ThrowIfFailed(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,next->allocator.get(),pipeline.get(),IID_PPV_ARGS(next->commands.put())));
            DX::ThrowIfFailed(next->commands->Close());
            next->root=root;next->pipeline=pipeline;slot=std::move(next);
        }
        slot->input=slot->ready=slot->consumed=0;
        slot->environment.reset();
        if(skyVisible&&environment&&environment->reflection&&environment->stamp.encoding==encoding&&
            environment->reflection->IsReadyFor(environment->stamp)&&
            Policy::PublicationFresh(true,environment->reflection->stamp.engineFrame,world.engineFrame)&&
            environment->producerFence&&MaterialFrameContract::ValidProducerSignal(environment->producerValue,environment->producerFence->GetCompletedValue()))
            slot->environment=std::move(environment);
        DX::ThrowIfFailed(slot->allocator->Reset());
        DX::ThrowIfFailed(slot->commands->Reset(slot->allocator.get(),pipeline.get()));
        auto* cmd=slot->commands.get();
        const auto step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto cpu=slot->descriptors->GetCPUDescriptorHandleForHeapStart();
        auto view=[&](ID3D12Resource* resource,DXGI_FORMAT fmt,UINT index){
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=fmt;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;
            device->CreateShaderResourceView(resource,&d,{cpu.ptr+SIZE_T(index)*step});
        };
        view(slot->raw->resource.get(),DXGI_FORMAT_R32G32B32A32_FLOAT,0);
        view(slot->mask->resource.get(),DXGI_FORMAT_R32_FLOAT,1);
        const auto sky=slot->environment?slot->environment->reflection:nullptr;
        D3D12_SHADER_RESOURCE_VIEW_DESC cube{};cube.Format=sky?sky->format:DXGI_FORMAT_R16G16B16A16_FLOAT;
        cube.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE;cube.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        cube.TextureCube.MipLevels=sky?sky->mipCount:1;
        device->CreateShaderResourceView(sky?sky->resource.get():nullptr,&cube,{cpu.ptr+SIZE_T(2)*step});
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(slot->result->resource.get(),nullptr,&uav,{cpu.ptr+SIZE_T(3)*step});
        const auto common=D3D12_RESOURCE_STATE_COMMON,read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,write=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Transition(cmd,slot->raw->resource.get(),common,read);Transition(cmd,slot->mask->resource.get(),common,read);
        if(sky)Transition(cmd,sky->resource.get(),common,read);
        Transition(cmd,slot->result->resource.get(),common,write);
        ID3D12DescriptorHeap* heaps[]{slot->descriptors.get()};cmd->SetDescriptorHeaps(1,heaps);cmd->SetComputeRootSignature(root.get());
        cmd->SetComputeRootDescriptorTable(0,slot->descriptors->GetGPUDescriptorHandleForHeapStart());
        const Constants constants{world.camera.CameraProj,world.camera.CameraProjInverse,world.camera.CameraViewInverse,
            world.width,world.height,ow,oh,uint32_t(world.reverseZ),uint32_t(bool(sky)),uint32_t(encoding),uint32_t(gammaInput),uint32_t(performance)};
        cmd->SetComputeRoot32BitConstants(1,kConstants,&constants,0);
        cmd->Dispatch((ow+kGroup-1)/kGroup,(oh+kGroup-1)/kGroup,1);
        Transition(cmd,slot->raw->resource.get(),read,common);Transition(cmd,slot->mask->resource.get(),read,common);
        if(sky)Transition(cmd,sky->resource.get(),read,common);
        Transition(cmd,slot->result->resource.get(),write,common);
        DX::ThrowIfFailed(cmd->Close());
        // Copy raw shader inputs using the original per-geometry constants; no
        // reflection evaluation takes place in the DX11 capture shader.
        const CaptureConstants cap{world.width,world.height,scale.x,scale.y};
        context->UpdateSubresource(captureCB.get(),0,nullptr,&cap,0,0);
        winrt::com_ptr<ID3D11ComputeShader> oldCS;context->CSGetShader(oldCS.put(),nullptr,nullptr);
        ID3D11ShaderResourceView* oldReads[3]{};context->CSGetShaderResources(0,3,oldReads);
        ID3D11UnorderedAccessView* oldWrites[2]{};context->CSGetUnorderedAccessViews(0,2,oldWrites);
        ID3D11Buffer* oldCB[3]{};UINT oldFirst[3]{},oldCount[3]{};
        context->CSGetConstantBuffers1(0,3,oldCB,oldFirst,oldCount);
        ID3D11UnorderedAccessView* writes[]{slot->raw->uav,slot->mask->uav};
        ID3D11Buffer* buffers[]{captureCB.get(),nullptr,geometry.get()};
        context->CSSetShaderResources(0,3,reads);
        context->CSSetUnorderedAccessViews(0,2,writes,nullptr);
        context->CSSetConstantBuffers(0,3,buffers);
        context->CSSetConstantBuffers1(2,1,&buffers[2],&firstConstant,&numConstants);context->CSSetShader(capture.get(),nullptr,0);
        ID3D11ShaderResourceView* check[3]{};context->CSGetShaderResources(0,3,check);
        bool bound=true;for(UINT i=0;i<3;++i){bound=bound&&check[i]==reads[i];if(check[i])check[i]->Release();}
        if(bound){slot->input=UINT64_MAX;context->Dispatch((world.width+kGroup-1)/kGroup,(world.height+kGroup-1)/kGroup,1);}
        context->CSSetUnorderedAccessViews(0,2,oldWrites,nullptr);context->CSSetShaderResources(0,3,oldReads);
        context->CSSetConstantBuffers1(0,3,oldCB,oldFirst,oldCount);context->CSSetShader(oldCS.get(),nullptr,0);
        for(auto* v:oldReads)if(v)v->Release();for(auto* v:oldWrites)if(v)v->Release();for(auto* v:oldCB)if(v)v->Release();
        if(!bound){++rejected;return;}
        auto* context4=owner.d3d11Context.get();
        const auto inputValue=inputFence.Next();
        DX::ThrowIfFailed(context4->Signal(inputFence.fence11.get(),inputValue));slot->input=inputValue;
        context4->Flush();
        DX::ThrowIfFailed(owner.commandQueue->Wait(inputFence.fence12.get(),inputValue));
        if(sky)DX::ThrowIfFailed(owner.commandQueue->Wait(slot->environment->producerFence.get(),slot->environment->producerValue));
        slot->ready=UINT64_MAX;
        ID3D12CommandList* lists[]{cmd};owner.commandQueue->ExecuteCommandLists(1,lists);
        const auto outputValue=outputFence.Next();
        DX::ThrowIfFailed(owner.commandQueue->Signal(outputFence.fence12.get(),outputValue));slot->ready=outputValue;
        DX::ThrowIfFailed(context4->Wait(outputFence.fence11.get(),outputValue));
        previousShader=boundShader;context->PSGetShaderResources(kReturnSlot,1,previousView.put());
        slot->consumed=UINT64_MAX;active=slot;
        auto* result=slot->result->srv;context->PSSetShaderResources(kReturnSlot,1,&result);
        context->PSSetShader(returnShader.get(),nullptr,0);
        ++submissions;failure.clear();
    } catch(const std::exception& e) {
        if(failure!=e.what())logger::warn("[NativeCubemaps] Water SSR: {}",e.what());
        failure=e.what();++rejected;
        if(active)End();
        // A failed recording cannot be reused. Submitted/untracked work is
        // deliberately quarantined until device removal or process exit.
        if(selected&&*selected&&(*selected)->input==0&&(*selected)->ready==0)selected->reset();
    }
}
void NativeCubemaps::WaterSSR::End()
{
    if(!active)return;
    auto* context=owner.d3d11Context.get();
    winrt::com_ptr<ID3D11PixelShader> ps;context->PSGetShader(ps.put(),nullptr,nullptr);
    if(ps.get()==returnShader.get())context->PSSetShader(previousShader.get(),nullptr,0);
    winrt::com_ptr<ID3D11ShaderResourceView> view;context->PSGetShaderResources(kReturnSlot,1,view.put());
    if(view.get()==active->result->srv){auto* previous=previousView.get();context->PSSetShaderResources(kReturnSlot,1,&previous);}
    const auto value=consumedFence.Next();
    if(SUCCEEDED(context->Signal(consumedFence.fence11.get(),value)))active->consumed=value;
    context->Flush();active.reset();previousShader=nullptr;previousView=nullptr;
}
json NativeCubemaps::WaterSSR::Diagnostics() const
{
    uint64_t bytes=0;for(const auto& slot:slots)if(slot)bytes+=slot->bytes;
    return {{"submissions",submissions},{"rejected",rejected},{"allocatedBytes",bytes},{"failure",failure}};
}

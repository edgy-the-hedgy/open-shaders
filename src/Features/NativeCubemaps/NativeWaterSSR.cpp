#include "NativeWaterSSR.h"
#include "NativeCubemapPolicy.h"
#include "Features/DX12/DX12FullNativeReconstruction.h"
#include <cstring>
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
        Matrix projection,inverseProjection,inverseView,view;
        float4 positionAdjust;
        uint32_t width,height,outputWidth,outputHeight;
        uint32_t sky,encoding,quality,frame;
        uint32_t instanceCount,characterEnabled,rayPitch,tokenCount;
        float emitterRadius;
        uint32_t hasHistory,padding[2]{};
        Matrix previousViewProjection;
        float4 previousPositionAdjust;
        float4 secondarySunDirection,secondarySunRadiance;
        uint32_t secondaryLightCount,secondaryFoliage,secondaryMaterialControls,secondaryProjectionRows;
        uint32_t characterVertices,characterTriangles,characterParts,characterPadding=0;
    };
    static_assert(sizeof(Constants)==480);
    constexpr UINT kTextureOffset=7;
    constexpr UINT kHistoryOffset=kTextureOffset+NativePBR::kGpuTextureDescriptorCapacity;
    constexpr UINT kProjectionOffset=kHistoryOffset+3;
    constexpr UINT kCharacterTextureOffset=kProjectionOffset+1+kProjectedTextureCapacity;
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
    std::unique_ptr<WrappedResource> raw,mask,result,sparse;
    winrt::com_ptr<ID3D12Resource> constants;
    std::shared_ptr<const DirectLightingFrame> direct;
    std::shared_ptr<const FullNativeReconstructionFrame> native;
    std::shared_ptr<Slot> historyInput;
    Policy::WaterHistoryIdentity historyIdentity{};
    Matrix viewProjection;
    float4 positionAdjust;
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
    latest.reset();
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
    CD3DX12_DESCRIPTOR_RANGE ranges[6];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,5,0,0,0);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,5);
    ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,NativePBR::kGpuTextureDescriptorCapacity,9,0,kTextureOffset);
    ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,525,0,kHistoryOffset);
    ranges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1+kProjectedTextureCapacity,530,0,kProjectionOffset);
    ranges[5].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,NativePBR::kGpuTextureDescriptorCapacity,4,1,kCharacterTextureOffset);
    CD3DX12_ROOT_PARAMETER params[17];
    params[0].InitAsDescriptorTable(6,ranges);
    params[1].InitAsConstantBufferView(0);
    params[2].InitAsConstants(1,1);
    for(UINT i=0;i<4;++i)params[3+i].InitAsShaderResourceView(5+i);
    for(UINT i=0;i<4;++i)params[7+i].InitAsShaderResourceView(521+i);
    params[11].InitAsShaderResourceView(528);
    params[12].InitAsShaderResourceView(529);
    for(UINT i=0;i<4;++i)params[13+i].InitAsShaderResourceView(i,1);
    std::array<CD3DX12_STATIC_SAMPLER_DESC,4> samplers;
    for(UINT i=0;i<samplers.size();++i)
        samplers[i]=CD3DX12_STATIC_SAMPLER_DESC(i,D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            (i&2)?D3D12_TEXTURE_ADDRESS_MODE_WRAP:D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            (i&1)?D3D12_TEXTURE_ADDRESS_MODE_WRAP:D3D12_TEXTURE_ADDRESS_MODE_CLAMP,D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    const CD3DX12_ROOT_SIGNATURE_DESC signature(17,params,UINT(samplers.size()),samplers.data());
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
    for(auto& slot:slots) if(slot && GpuFenceRetired(outputFence.fence12.get(),slot->ready)) { slot->environment.reset(); slot->native.reset(); slot->direct.reset(); slot->historyInput.reset(); }
}

void NativeCubemaps::WaterSSR::Draw(const GraphicsBackend& backend,const NativeWorldFrame& world,std::shared_ptr<const EnvironmentFrame> environment,
    std::shared_ptr<const DirectLightingFrame> direct,std::shared_ptr<const FullNativeReconstructionFrame> native,EnvironmentEncoding encoding,bool skyVisible,uint32_t quality,float emitterRadius,ID3D11PixelShader* expected)
{
    using namespace MaterialFrameContract;
    if (active || !expected || world.width<2 || !world.height || !globals::d3d::context) return;
    if(!direct || direct->frame!=world.engineFrame || direct->width!=world.width || direct->height!=world.height ||
        direct->quality!=SanitizeRTQuality(quality) || !direct->material || !direct->lights || direct->lightCount>DX12::kMaxPreparedLights ||
        !direct->material->IsCoherent(world.engineFrame,world.width,world.height) || !direct->readyFence ||
        !ValidProducerSignal(direct->readyValue,direct->readyFence->GetCompletedValue())){++rejected;return;}
    const auto& material=*direct->material;
    const auto stamp=RasterIdentityStamp{world.engineFrame,material.scope,material.geometryRevision,material.materialRevision};
    // Optional screen hit reuse. Never require opaque reconstruction or the
    // first completed environment generation to trace a water reflection.
    if(!native || native->material!=direct->material || native->direct!=direct || native->frame!=world.engineFrame ||
        native->quality!=direct->quality || native->width!=world.width || native->height!=world.height ||
        !native->environmentRadiance || native->environmentRadiance->GetDesc().Width<uint64_t(world.width)*world.height*8u ||
        !native->readyFence || !ValidProducerSignal(native->readyValue,native->readyFence->GetCompletedValue()))
        native.reset();
    auto evidence=native?native->rasterEvidence:nullptr;
    if(!evidence || evidence->stamp!=stamp || !evidence->tokenCount || !evidence->tokenTable ||
       !evidence->texture || !evidence->texture->resource || evidence->width<world.width || evidence->height<world.height ||
       !evidence->readyFence || !ValidProducerSignal(evidence->readyValue,evidence->readyFence->GetCompletedValue())){
        evidence.reset();native.reset();
    }
    const auto characters=direct->characters;
    const auto& scene=*material.scene;
    const auto& evaluation=*scene.materialEvaluation;
    quality=SanitizeRTQuality(quality);
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
    emitterRadius=std::isfinite(emitterRadius)?std::clamp(emitterRadius,0.0f,1.0f):0.0f;
    std::array<winrt::com_ptr<ID3D11ShaderResourceView>,2> input;
    context->PSGetShaderResources(0,1,input[0].put());
    context->PSGetShaderResources(2,1,input[1].put());
    ID3D11ShaderResourceView* reads[]{input[0].get(),input[1].get()};
    if(!std::ranges::all_of(input,[](const auto& v){return TextureInput(v.get());})){++rejected;return;}
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
        for(auto& slot:slots) if(!slot || (slot.use_count()==1 && GpuFenceRetired(inputFence.fence12.get(),slot->input)&&
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
            const auto a=alloc(DXGI_FORMAT_R32_FLOAT,world.width,world.height);
            const auto b=alloc(DXGI_FORMAT_R32_FLOAT,world.width,world.height);
            const auto c=alloc(DXGI_FORMAT_R16G16B16A16_FLOAT,ow,oh);
            const auto d=alloc(DXGI_FORMAT_R16G16B16A16_FLOAT,ow,oh);
            if(a==UINT64_MAX||b==UINT64_MAX||c==UINT64_MAX||d==UINT64_MAX||a>UINT64_MAX-b||a+b>UINT64_MAX-c||a+b+c>UINT64_MAX-d)
                throw std::runtime_error("Water SSR allocation unsupported");
            uint64_t tracked=0;for(const auto& old:slots)if(old)tracked+=old->bytes;
            const auto allowance=budget.Query(device,tracked).effectiveBytes;
            if(a+b+c+d>allowance||tracked>allowance-(a+b+c+d))throw std::runtime_error("Water SSR deferred by DXGI budget");
            auto next=std::make_shared<Slot>();
            auto texture=[&](DXGI_FORMAT fmt,uint32_t w,uint32_t h,const char* name){
                D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.ArraySize=1;desc.Format=fmt;
                desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
                return std::make_unique<WrappedResource>(desc,owner.d3d11Device.get(),device,name);
            };
            next->raw=texture(DXGI_FORMAT_R32_FLOAT,world.width,world.height,"NativeWaterSSR::Depth");
            next->mask=texture(DXGI_FORMAT_R32_FLOAT,world.width,world.height,"NativeWaterSSR::Mask");
            next->result=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,ow,oh,"NativeWaterSSR::Result");
            next->sparse=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,ow,oh,"NativeWaterSSR::Sparse");
            next->constants=NativeMaterialEvaluation::Buffer(device,512,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            next->width=world.width;next->height=world.height;next->outWidth=ow;next->outHeight=oh;next->bytes=a+b+c+d;
            const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,kCharacterTextureOffset+NativePBR::kGpuTextureDescriptorCapacity,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            DX::ThrowIfFailed(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(next->descriptors.put())));
            DX::ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(next->allocator.put())));
            DX::ThrowIfFailed(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,next->allocator.get(),pipeline.get(),IID_PPV_ARGS(next->commands.put())));
            DX::ThrowIfFailed(next->commands->Close());
            next->root=root;next->pipeline=pipeline;slot=std::move(next);
        }
        slot->input=slot->ready=slot->consumed=0;
        slot->environment.reset();slot->direct=direct;slot->native=native;
        const auto fallbackCube=environment ? (skyVisible && environment->reflection ? environment->reflection : environment->base) : nullptr;
        if(environment&&fallbackCube&&environment->stamp.scope==material.scope&&environment->stamp.encoding==encoding&&
            fallbackCube->IsReadyFor(environment->stamp)&&
            Policy::PublicationFresh(true,fallbackCube->stamp.engineFrame,world.engineFrame)&&
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
        view(slot->raw->resource.get(),DXGI_FORMAT_R32_FLOAT,0);
        view(slot->mask->resource.get(),DXGI_FORMAT_R32_FLOAT,1);
        const auto sky=slot->environment?fallbackCube:nullptr;
        const Policy::WaterHistoryIdentity identity{world.engineFrame,world.pluginFrame,world.width,world.height,ow,oh,
            material.scope,quality,uint32_t(encoding),world.cameraIdentity,material.geometryRevision,material.materialRevision,
            slot->environment?slot->environment->stamp.epoch:0u,sky?sky->revision:0u,emitterRadius,world.resetHistory};
        const auto previous=latest && identity.CanReuse(latest->historyIdentity) &&
            GpuFenceRetired(outputFence.fence12.get(),latest->ready) &&
            GpuFenceRetired(consumedFence.fence12.get(),latest->consumed) ? latest : nullptr;
        slot->historyInput=previous;slot->historyIdentity=identity;
        slot->viewProjection=world.camera.CameraViewProj;slot->positionAdjust=world.camera.CameraPosAdjust;
        view(previous?previous->raw->resource.get():nullptr,DXGI_FORMAT_R32_FLOAT,kHistoryOffset);
        view(previous?previous->mask->resource.get():nullptr,DXGI_FORMAT_R32_FLOAT,kHistoryOffset+1);
        view(previous?previous->result->resource.get():nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT,kHistoryOffset+2);
        auto* opaqueDepth=material.raster->depth->resource.get();
        auto* tokens=evidence?evidence->texture->resource.get():nullptr;
        view(opaqueDepth,DXGI_FORMAT_R32_FLOAT,3);
        view(tokens,DXGI_FORMAT_R16G16B16A16_UNORM,4);
        evaluation.WriteTextureViews(device,{cpu.ptr+SIZE_T(kTextureOffset)*step});
        const auto* actor=characters?characters->ReflectionActor():nullptr;
        const auto& actorEvaluation=actor?*actor->materials->evaluation:evaluation;
        actorEvaluation.WriteTextureViews(device,{cpu.ptr+SIZE_T(kCharacterTextureOffset)*step});
        const auto projection=material.projection && material.projectionLookup && material.projection->Ready(stamp)?material.projection:nullptr;
        WriteProjectionViews(device,projection.get(),{cpu.ptr+SIZE_T(kProjectionOffset)*step});
        D3D12_SHADER_RESOURCE_VIEW_DESC cube{};cube.Format=sky?sky->format:DXGI_FORMAT_R16G16B16A16_FLOAT;
        cube.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE;cube.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        cube.TextureCube.MipLevels=sky?sky->mipCount:1;
        device->CreateShaderResourceView(sky?sky->resource.get():nullptr,&cube,{cpu.ptr+SIZE_T(2)*step});
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(slot->result->resource.get(),nullptr,&uav,{cpu.ptr+SIZE_T(5)*step});
        device->CreateUnorderedAccessView(slot->sparse->resource.get(),nullptr,&uav,{cpu.ptr+SIZE_T(6)*step});
        const auto common=D3D12_RESOURCE_STATE_COMMON,read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,write=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Transition(cmd,slot->raw->resource.get(),common,read);Transition(cmd,slot->mask->resource.get(),common,read);
        if(sky)Transition(cmd,sky->resource.get(),common,read);
        Transition(cmd,slot->result->resource.get(),common,write);
        Transition(cmd,slot->sparse->resource.get(),common,write);
        Transition(cmd,opaqueDepth,common,read);if(tokens)Transition(cmd,tokens,common,read);
        if(previous){Transition(cmd,previous->raw->resource.get(),common,read);Transition(cmd,previous->mask->resource.get(),common,read);Transition(cmd,previous->result->resource.get(),common,read);}
        ID3D12DescriptorHeap* heaps[]{slot->descriptors.get()};cmd->SetDescriptorHeaps(1,heaps);cmd->SetComputeRootSignature(root.get());
        cmd->SetComputeRootDescriptorTable(0,slot->descriptors->GetGPUDescriptorHandleForHeapStart());
        const Constants constants{world.camera.CameraProj,world.camera.CameraProjInverse,world.camera.CameraViewInverse,world.camera.CameraView,
            world.camera.CameraPosAdjust,world.width,world.height,ow,oh,uint32_t(bool(sky)),uint32_t(encoding),quality,world.engineFrame,
            evaluation.instanceCount,uint32_t(bool(characters)),material.rayIdentityRowPitch,evidence?evidence->tokenCount:0u,
            emitterRadius,uint32_t(bool(previous)),{},previous?previous->viewProjection:Matrix{},previous?previous->positionAdjust:float4{},
            direct->directional.direction,direct->directional.radiance,
            direct->lightCount,uint32_t(direct->foliageScattering),
            uint32_t(material.parallaxEnabled)|(uint32_t(material.parallaxShadows)<<1u),projection?projection->rowCount:0u,
            actor?actor->vertices:0u,actor?actor->triangles:0u,actor?actor->parts:0u};
        void* mapped=nullptr;const D3D12_RANGE noRead{};
        DX::ThrowIfFailed(slot->constants->Map(0,&noRead,&mapped));
        std::memcpy(mapped,&constants,sizeof(constants));slot->constants->Unmap(0,nullptr);
        cmd->SetComputeRootConstantBufferView(1,slot->constants->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(3,scene.acceleration->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(4,scene.shadingGeometry->buffer->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(5,evaluation.records->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(6,evaluation.bindings->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(7,characters?characters->acceleration->GetGPUVirtualAddress():scene.acceleration->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(8,material.rayIdentity->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(9,evidence?evidence->tokenTable->GetGPUVirtualAddress():material.rayIdentity->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(10,native?native->environmentRadiance->GetGPUVirtualAddress():material.samples->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(11,direct->lights->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(12,projection?material.projectionLookup->GetGPUVirtualAddress():material.samples->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(13,actor?actor->positions->GetGPUVirtualAddress():material.samples->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(14,actor?actor->indices->GetGPUVirtualAddress():material.samples->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(15,actorEvaluation.records->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(16,actorEvaluation.bindings->GetGPUVirtualAddress());
        TransitionProjectionViews(cmd,projection.get(),common,read);
        cmd->SetComputeRoot32BitConstant(2,0,0);
        cmd->Dispatch((ShadowPhysicalWidth(ShadowLogicalExtent(ow,quality),quality)+kGroup-1)/kGroup,
            (ShadowLogicalExtent(oh,quality)+kGroup-1)/kGroup,1);
        const auto barrier=CD3DX12_RESOURCE_BARRIER::UAV(slot->sparse->resource.get());
        cmd->ResourceBarrier(1,&barrier);
        cmd->SetComputeRoot32BitConstant(2,1,0);
        cmd->Dispatch((ow+kGroup-1)/kGroup,(oh+kGroup-1)/kGroup,1);
        TransitionProjectionViews(cmd,projection.get(),read,common);
        Transition(cmd,slot->raw->resource.get(),read,common);Transition(cmd,slot->mask->resource.get(),read,common);
        if(sky)Transition(cmd,sky->resource.get(),read,common);
        Transition(cmd,slot->result->resource.get(),write,common);
        Transition(cmd,slot->sparse->resource.get(),write,common);
        Transition(cmd,opaqueDepth,read,common);if(tokens)Transition(cmd,tokens,read,common);
        if(previous){Transition(cmd,previous->raw->resource.get(),read,common);Transition(cmd,previous->mask->resource.get(),read,common);Transition(cmd,previous->result->resource.get(),read,common);}
        DX::ThrowIfFailed(cmd->Close());
        // Capture only engine water coverage and native-depth values.
        const CaptureConstants cap{world.width,world.height,scale.x,scale.y};
        context->UpdateSubresource(captureCB.get(),0,nullptr,&cap,0,0);
        winrt::com_ptr<ID3D11ComputeShader> oldCS;context->CSGetShader(oldCS.put(),nullptr,nullptr);
        ID3D11ShaderResourceView* oldReads[2]{};context->CSGetShaderResources(0,2,oldReads);
        ID3D11UnorderedAccessView* oldWrites[2]{};context->CSGetUnorderedAccessViews(0,2,oldWrites);
        winrt::com_ptr<ID3D11Buffer> oldCB;UINT oldFirst=0,oldCount=0;
        context->CSGetConstantBuffers1(0,1,oldCB.put(),&oldFirst,&oldCount);
        ID3D11UnorderedAccessView* writes[]{slot->raw->uav,slot->mask->uav};
        auto* buffer=captureCB.get();
        context->CSSetShaderResources(0,2,reads);
        context->CSSetUnorderedAccessViews(0,2,writes,nullptr);
        context->CSSetConstantBuffers(0,1,&buffer);context->CSSetShader(capture.get(),nullptr,0);
        ID3D11ShaderResourceView* check[2]{};context->CSGetShaderResources(0,2,check);
        ID3D11UnorderedAccessView* checkWrites[2]{};context->CSGetUnorderedAccessViews(0,2,checkWrites);
        bool bound=true;for(UINT i=0;i<2;++i){
            bound=bound&&check[i]==reads[i]&&checkWrites[i]==writes[i];
            if(check[i])check[i]->Release();if(checkWrites[i])checkWrites[i]->Release();
        }
        if(bound){slot->input=UINT64_MAX;context->Dispatch((world.width+kGroup-1)/kGroup,(world.height+kGroup-1)/kGroup,1);}
        context->CSSetUnorderedAccessViews(0,2,oldWrites,nullptr);context->CSSetShaderResources(0,2,oldReads);
        buffer=oldCB.get();context->CSSetConstantBuffers1(0,1,&buffer,&oldFirst,&oldCount);context->CSSetShader(oldCS.get(),nullptr,0);
        for(auto* v:oldReads)if(v)v->Release();for(auto* v:oldWrites)if(v)v->Release();
        if(!bound){++rejected;return;}
        auto* context4=owner.d3d11Context.get();
        const auto inputValue=inputFence.Next();
        DX::ThrowIfFailed(context4->Signal(inputFence.fence11.get(),inputValue));slot->input=inputValue;
        context4->Flush();
        DX::ThrowIfFailed(owner.commandQueue->Wait(inputFence.fence12.get(),inputValue));
        if(sky)DX::ThrowIfFailed(owner.commandQueue->Wait(slot->environment->producerFence.get(),slot->environment->producerValue));
        DX::ThrowIfFailed(owner.commandQueue->Wait(direct->readyFence.get(),direct->readyValue));
        if(native)DX::ThrowIfFailed(owner.commandQueue->Wait(native->readyFence.get(),native->readyValue));
        if(evidence)DX::ThrowIfFailed(owner.commandQueue->Wait(evidence->readyFence.get(),evidence->readyValue));
        if(projection)DX::ThrowIfFailed(owner.commandQueue->Wait(projection->readyFence.get(),projection->readyValue));
        if(characters)DX::ThrowIfFailed(owner.commandQueue->Wait(characters->readyFence.get(),characters->readyValue));
        slot->ready=UINT64_MAX;
        ID3D12CommandList* lists[]{cmd};owner.commandQueue->ExecuteCommandLists(1,lists);
        const auto outputValue=outputFence.Next();
        DX::ThrowIfFailed(owner.commandQueue->Signal(outputFence.fence12.get(),outputValue));slot->ready=outputValue;
        DX::ThrowIfFailed(context4->Wait(outputFence.fence11.get(),outputValue));
        previousShader=boundShader;context->PSGetShaderResources(kReturnSlot,1,previousView.put());
        slot->consumed=UINT64_MAX;active=slot;
        auto* result=slot->result->srv;context->PSSetShaderResources(kReturnSlot,1,&result);
        context->PSSetShader(returnShader.get(),nullptr,0);
        latest=slot;
        ++submissions;historySubmissions+=uint64_t(bool(previous));failure.clear();
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
    return {{"submissions",submissions},{"historySubmissions",historySubmissions},{"rejected",rejected},{"allocatedBytes",bytes},{"failure",failure}};
}

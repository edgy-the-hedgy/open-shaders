#include "NativeCubemaps.h"
#include "NativeCubemapPolicy.h"
#include "Features/DX12/DX12FullNativeReconstruction.h"
#include "Features/DX12/DX12NativeScene.h"

#include "Features/NativeIBL/NativeIBL.h"
#include "Features/DX12/DX12Control.h"
#include "Features/Upscaling.h"
#include "ShaderCache.h"
#include "Features/LinearLighting.h"
#include "Features/ReverseZ.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/FileSystem.h"
#include "Utils/Game.h"
#include "GpuPass.h"
#include <cmath>
#include <stdexcept>

namespace
{
    using NativeCubemaps::Policy::kSize;
    struct RawConstants {
        Matrix view, projection, inverseViewProjection;
        float4 dynamicResolution;
        float farDepth;
        uint32_t size,padding[2]{};
    };
    static_assert(sizeof(RawConstants)%16==0);
}
DX12Native::EnvironmentStamp NativeCubemaps::Producer::Prepare(uint32_t scope,uint32_t frame,bool& changed)
{
    using namespace DX12Native;
    const auto color=globals::features::linearLighting.GetCommonBufferData();
    const auto encoding=color.enableLinearLighting ?
        (color.enableACEScg?EnvironmentEncoding::LinearAP1:EnvironmentEncoding::LinearRec709):EnvironmentEncoding::LegacyScene;
    const bool interior=Util::IsInterior();
    const bool skyHidden=!interior && (!globals::game::sky || globals::game::sky->flags.any(RE::Sky::Flags::kHideSky));
    EnvironmentStamp stamp{epoch,scope,frame,globals::state->frameCount,encoding,interior,skyHidden};
    const float hours=globals::game::calendar?globals::game::calendar->GetHoursPassed():lastHours;
    changed=currentStamp.epoch && (!currentStamp.SameEnvironment(stamp) ||
        Policy::ClockDiscontinuity(lastHours,hours));
    if(changed) { InvalidateGeneration(); stamp.epoch=epoch; }
    currentStamp=stamp;
    if(processing) processing->Poll();
    if(waterSSR) waterSSR->Poll();
    if(std::isfinite(hours)) lastHours=hours;
    return stamp;
}
bool NativeCubemaps::Producer::CaptureRaw(const GraphicsBackend& backend, ID3D11ShaderResourceView* sceneColor)
{
    CS_GPU_PASS("NativeCubemaps::RawInput");
    auto* renderer=globals::game::renderer;
    if(!renderer || !backend.d3d11Device || !backend.d3d11Context) return false;
    auto* device=backend.d3d11Device.get();
    auto* context=backend.d3d11Context.get();
    if(!raw || !inputShader || !rawUAV || !inputConstants) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=kSize; desc.Height=kSize; desc.MipLevels=1; desc.ArraySize=6;
        desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11UnorderedAccessView> uav;
        winrt::com_ptr<ID3D11Buffer> constants;
        DX::ThrowIfFailed(device->CreateTexture2D(&desc,nullptr,texture.put()));
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format=desc.Format; view.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2DARRAY; view.Texture2DArray.ArraySize=6;
        DX::ThrowIfFailed(device->CreateUnorderedAccessView(texture.get(),&view,uav.put()));
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth=sizeof(RawConstants); cb.Usage=D3D11_USAGE_DEFAULT; cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        DX::ThrowIfFailed(device->CreateBuffer(&cb,nullptr,constants.put()));
        const auto path=Util::PathHelpers::GetShadersPath()/"NativeCubemaps"/"CaptureRaw.cs.hlsl";
        winrt::com_ptr<ID3D11ComputeShader> shader;
        shader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(),{},"cs_5_0")));
        if(!shader) return false;
        raw=std::move(texture); rawUAV=std::move(uav); inputConstants=std::move(constants); inputShader=std::move(shader);
    }
    auto& depth=renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
    ID3D11ShaderResourceView* reads[]{sceneColor,Util::AsReal(depth.depthSRV)};
    if(!reads[0] || !reads[1]) return false;
    const auto world=globals::features::dx12NativeScene.GetWorldFrame(currentStamp.engineFrame,currentStamp.pluginFrame);
    if(!world) return false;
    const auto& camera=world->camera;
    const RawConstants data{camera.CameraView,camera.CameraProj,camera.CameraViewProjInverse,
        camera.DynamicResolutionParams1,globals::features::reverseZ.GetFarDepth(),kSize};
    context->UpdateSubresource(inputConstants.get(),0,nullptr,&data,0,0);
    winrt::com_ptr<ID3D11ComputeShader> oldShader;
    winrt::com_ptr<ID3D11Buffer> oldCB;
    winrt::com_ptr<ID3D11UnorderedAccessView> oldUAV;
    ID3D11ShaderResourceView* oldReads[2]{};
    context->CSGetShader(oldShader.put(),nullptr,nullptr);
    context->CSGetConstantBuffers(0,1,oldCB.put());
    context->CSGetUnorderedAccessViews(0,1,oldUAV.put());
    context->CSGetShaderResources(0,2,oldReads);
    context->CSSetShaderResources(0,2,reads);
    ID3D11ShaderResourceView* bound[2]{};
    context->CSGetShaderResources(0,2,bound);
    const bool valid=bound[0]==reads[0] && bound[1]==reads[1];
    for(auto* view:bound) if(view) view->Release();
    if(valid) {
        auto* uav=rawUAV.get(); auto* cb=inputConstants.get();
        context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        context->CSSetConstantBuffers(0,1,&cb);
        context->CSSetShader(inputShader.get(),nullptr,0);
        context->Dispatch(kSize/8,kSize/8,6);
    }
    auto* uav=oldUAV.get(); auto* cb=oldCB.get();
    context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    context->CSSetConstantBuffers(0,1,&cb);
    context->CSSetShader(oldShader.get(),nullptr,0);
    context->CSSetShaderResources(0,2,oldReads);
    for(auto* view:oldReads) if(view) view->Release();
    return valid;
}
void NativeCubemaps::Producer::Capture(const GraphicsBackend& backend, ID3D11ShaderResourceView* sceneColor, uint32_t frame,
    std::shared_ptr<const DX12Native::FullNativeReconstructionFrame> native)
{
    using namespace DX12Native;
    if(!loaded || !currentStamp.scope || currentStamp.engineFrame != frame || !globals::state ||
        currentStamp.pluginFrame != globals::state->frameCount || !globals::game::renderer) return;
    const auto world=globals::features::dx12NativeScene.GetWorldFrame(frame,currentStamp.pluginFrame);
    if(!world) return;
    try {
        if(processing) {
            processing->Advance(frame);
            if(processing->Pending()) return;
        }
        if(!Policy::CaptureDue(lastCaptureFrame,currentStamp.engineFrame,globals::features::dx12Control.NativeCubemapCaptureInterval())) return;
        // A depth match alone is not sufficient for native radiance acquisition:
        // require a same-frame raster identity candidate and direct-light input.
        // This is environment observation, not final-color ownership.
        if (!native || native->frame != frame || !native->material ||
            native->material->scope != currentStamp.scope || !native->direct ||
            native->direct->material != native->material || !native->rasterEvidence ||
            !native->rasterEvidence->tokenCount || !native->ownershipMask || !native->environmentRadiance)
            native.reset();
        std::array<float4,3> dalc{};
        if(!globals::state->GetAmbientSH(currentStamp.engineFrame,currentStamp.pluginFrame,dalc)) return;
        EnvironmentSource source{};
        if(sceneColor) {
            try {
                if(CaptureRaw(backend,sceneColor)) source={raw.get(),++rawRevision,currentStamp};
            } catch(const std::exception& e) {
                if(!failureLogged) logger::warn("[NativeCubemaps] Raster compatibility capture unavailable: {}",e.what());
                failureLogged=true;
            }
        }
        if(!source.texture && !native) return;
        if(!inputs) inputs=std::make_unique<NativeEnvironment>();
        const auto sky=GetSkySource(currentStamp);
        EnvironmentSource reflection{};
        // The native reflection hook certifies complete face coverage for the
        // matching scene. Borrow the engine's unfiltered reflection target.
        if(sky.IsValidFor(currentStamp)) {
            const auto& target=globals::game::renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];
            reflection={Util::AsReal(target.texture),sky.revision,sky.stamp};
        }
        inputs->Capture(backend,currentStamp,source,reflection,sky,bool(native));
        auto captured=inputs->Get();
        if(!captured) return;
        if(captured->base && captured->base->revision!=source.revision) {
            auto fresh=std::make_shared<EnvironmentFrame>(*captured);
            fresh->base.reset();
            captured=std::move(fresh);
        }
        if(!captured->base && !native) return;
        if(!processing) processing=std::make_unique<CubeProcessing>();
        if(processing->Dispatch(backend,captured,dalc,world->camera.CameraPosAdjust,native)) {
            lastCaptureFrame=currentStamp.engineFrame;
            failureLogged=false;
        }
    } catch(const std::exception& e) {
        if(!failureLogged) logger::warn("[NativeCubemaps] {}",e.what());
        failureLogged=true;
    } catch(...) {
        if(!failureLogged) logger::warn("[NativeCubemaps] Environment input or processing failed");
        failureLogged=true;
    }
}
std::shared_ptr<const DX12Native::EnvironmentFrame> NativeCubemaps::Producer::Get() const
{
    const auto frame = processing ? processing->Get() : nullptr;
    if (!frame || !frame->base || !Policy::PublicationFresh(frame->stamp.SameEnvironment(currentStamp),
        frame->base->stamp.engineFrame, currentStamp.engineFrame))
        return nullptr;
    return frame;
}

void NativeCubemaps::Producer::SetupResources()
{
    if(loaded && globals::game::renderer) skyCapture.Setup();
}

std::function<void()> NativeCubemaps::Producer::OnReflectionsRenderBegin()
{
    using namespace DX12Native;
    if(!loaded || !globals::features::dx12Control.WantsMaterialPreparation()) return nullptr;
    auto stamp=currentStamp;
    const auto color=globals::features::linearLighting.GetCommonBufferData();
    const auto encoding=color.enableLinearLighting ?
        (color.enableACEScg?EnvironmentEncoding::LinearAP1:EnvironmentEncoding::LinearRec709):EnvironmentEncoding::LegacyScene;
    const bool visibleSky=!Util::IsInterior() && globals::game::sky &&
        !globals::game::sky->flags.any(RE::Sky::Flags::kHideSky);
    const bool enabled=visibleSky && globals::shaderCache && globals::shaderCache->IsEnabled() &&
        globals::game::graphicsState && globals::state && stamp.scope && stamp.encoding==encoding && !stamp.interior && !stamp.skyHidden;
    if(enabled&&!skyCapture.Allocated()) skyCapture.Setup();
    auto cleanup=skyCapture.Begin(enabled);
    if(!cleanup){skyStamp={};skyFaces={};return nullptr;}
    if(enabled){
        stamp.engineFrame=globals::game::graphicsState->GetFrameCount();
        stamp.pluginFrame=globals::state->frameCount;
    }else stamp={};
    return [this,stamp,cleanup=std::move(cleanup)]{
        cleanup();
        if(skyCapture.IsCapturing())return;
        if(!stamp.SameEnvironment(currentStamp)||skyCapture.Failed()){
            skyStamp={};skyFaces={};return;
        }
        const uint32_t updated=skyCapture.ClearedFaces();
        skyFaces.Observe(updated,stamp.engineFrame);
        if(!skyCapture.Ready()||!skyFaces.Fresh(stamp.engineFrame)) {skyStamp={};return;}
        if(updated){skyStamp=stamp;++skyRevision;}
    };
}

DX12Native::EnvironmentSource NativeCubemaps::Producer::GetSkySource(const DX12Native::EnvironmentStamp& requested) const
{
    if(requested.interior||requested.skyHidden||!skyFaces.Fresh(requested.engineFrame))return {};
    DX12Native::EnvironmentSource source{skyCapture.Ready()?skyCapture.Texture():nullptr,skyRevision,skyStamp};
    return source.IsValidFor(requested)&&Policy::PublicationFresh(true,skyStamp.engineFrame,requested.engineFrame)?
        source:DX12Native::EnvironmentSource{};
}

void NativeCubemaps::Producer::BeginForward()
{
    EndForward();
    if(!loaded || !globals::features::dx12Control.WantsMaterialPreparation() ||
        !globals::state || !globals::state->inWorld || globals::state->nativeFirstPersonDepth ||
        (globals::state->permutationData.ExtraShaderDescriptor & uint32_t(State::ExtraShaderDescriptors::IsReflections))) return;
    const auto* backend=globals::features::dx12NativeScene.GetNativeBackend();
    if(!backend || !globals::game::graphicsState) return;
    const uint32_t frame=globals::game::graphicsState->GetFrameCount();
    if(currentStamp.engineFrame!=frame || currentStamp.pluginFrame!=globals::state->frameCount) return;
    if(!forward) forward=std::make_unique<ForwardEnvironment>();
    forward->Update(*backend,Get(),currentStamp);
    if(forward->Bind(*backend,currentStamp))
        globals::state->permutationData.ExtraShaderDescriptor |= uint32_t(State::ExtraShaderDescriptors::NativeForwardEnvironment);
}
void NativeCubemaps::Producer::EndForward()
{
    if(globals::state) globals::state->permutationData.ExtraShaderDescriptor &= ~uint32_t(State::ExtraShaderDescriptors::NativeForwardEnvironment);
    if(forward) forward->Unbind();
}

void NativeCubemaps::Producer::BeginWaterSSR()
{
    if (!globals::features::dx12Control.WantsMaterialPreparation()) return;
    if (waterScopeDepth++) {
        waterScopeInvalid = true;
        if (waterSSR) waterSSR->End();
    } else waterScopeInvalid = false;
}
void NativeCubemaps::Producer::EndWaterSSR()
{
    if (!waterScopeDepth) return;
    if (--waterScopeDepth == 0) {
        if (waterSSR) waterSSR->End();
        waterScopeInvalid = false;
    }
}
void NativeCubemaps::Producer::OnDraw()
{
    skyCapture.OnDraw();
    if (waterScopeDepth != 1 || waterScopeInvalid ||
        !globals::features::dx12Control.NativeSSREnabled() || !globals::state ||
        !globals::game::graphicsState || !globals::state->currentShader ||
        globals::state->nativeFirstPersonDepth || globals::state->isMapMenuOpen ||
        (globals::state->permutationData.ExtraShaderDescriptor & uint32_t(State::ExtraShaderDescriptors::IsReflections))) return;
    const auto* shader = globals::state->currentShader;
    if (shader->shaderType.get() != RE::BSShader::Type::ImageSpace ||
        std::string_view(static_cast<const RE::BSImagespaceShader&>(*shader).originalShaderName) != "ISReflectionsRayTracing") return;
    const auto world = globals::features::dx12NativeScene.GetWorldFrame(
        globals::game::graphicsState->GetFrameCount(),globals::state->frameCount);
    const auto* backend=globals::features::dx12NativeScene.GetNativeBackend();
    if (!world || !backend || !globals::state->customPixelShader) return;
    if (!waterSSR) waterSSR = std::make_unique<WaterSSR>();
    const auto color=globals::features::linearLighting.GetCommonBufferData();
    const auto encoding=color.enableLinearLighting ?
        (color.enableACEScg?DX12Native::EnvironmentEncoding::LinearAP1:DX12Native::EnvironmentEncoding::LinearRec709):DX12Native::EnvironmentEncoding::LegacyScene;
    waterSSR->Draw(*backend,*world,Get(),globals::features::dx12NativeScene.GetFullNativeDirect(world->engineFrame),
        globals::features::upscaling.nativeComposite.GetFullNativeReconstruction(world->engineFrame),encoding,
        !Util::IsInterior() && globals::game::sky && !globals::game::sky->flags.any(RE::Sky::Flags::kHideSky),
        globals::features::dx12NativeScene.GetFullNativeQuality(world->engineFrame),
        globals::features::dx12Control.NativeWaterReflectionRadius(),
        Util::AsReal(globals::state->customPixelShader->shader));
}

json NativeCubemaps::Producer::GetDiagnostics()
{
    const auto publication=Get();
    return { {"nativeRadianceInputAdmitted",publication && publication->nativeRadianceInput},
        {"transitionalRasterInputAdmitted",publication && publication->rasterCompatibilityInput},
        {"lastCaptureFrame",lastCaptureFrame}, {"captureInterval",globals::features::dx12Control.NativeCubemapCaptureInterval()}, {"environmentRevision",publication ? publication->revision : 0},
        {"basePublished",publication && bool(publication->base)}, {"reflectionPublished",publication && bool(publication->reflection)},
        {"skyFacesValid",skyCapture.ValidFaces()}, {"skyRevision",skyRevision},
        {"skyAcquisitionFresh",bool(GetSkySource(currentStamp).texture)}, {"skyHidden",currentStamp.skyHidden},
        {"skyPublished",publication && bool(publication->sky)}, {"inputAllocationBytes",inputs ? inputs->AllocationBytes() : 0},
        {"inputFailure",inputs ? inputs->Failure() : ""},
        {"processing",processing ? processing->GetDiagnostics() : json::object()},
        {"waterSSR",waterSSR ? waterSSR->Diagnostics() : json::object()},
        {"forwardEnvironment",forward ? forward->Diagnostics() : json::object()} };
}

void NativeCubemaps::Producer::InvalidateGeneration()
{
    EndForward();
    if(forward) forward->Reset();
    if(waterSSR) { waterSSR->End(); waterSSR->ResetHistory(); }
    waterScopeDepth=0; waterScopeInvalid=false;
    if(processing) processing->Reset();
    if(inputs) inputs->Reset();
    skyCapture.Invalidate(); skyFaces={}; skyStamp={};
    currentStamp={}; ++epoch; lastCaptureFrame=UINT32_MAX; failureLogged=false;
}
void NativeCubemaps::Producer::Release()
{
    EndForward();
    skyCapture.Release(); skyFaces={}; skyStamp={};
    forward.reset(); waterSSR.reset();
    processing.reset(); inputs.reset();
    raw=nullptr; rawUAV=nullptr; inputShader=nullptr; inputConstants=nullptr;
    lastCaptureFrame=UINT32_MAX;
}

#include "NativeCubemaps.h"
#include "NativeCubemapPolicy.h"
#include "Features/DX12/DX12FullNativeReconstruction.h"
#include "Features/DX12/DX12NativeScene.h"

#include "Features/NativeIBL/NativeIBL.h"
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
    EnvironmentStamp stamp{epoch,scope,frame,globals::state->frameCount,encoding,Util::IsInterior()};
    const float hours=globals::game::calendar?globals::game::calendar->GetHoursPassed():lastHours;
    changed=currentStamp.epoch && (!currentStamp.SameEnvironment(stamp) ||
        (std::isfinite(hours) && std::abs(hours-lastHours)>0.01f));
    if(changed) { InvalidateGeneration(); stamp.epoch=epoch; }
    currentStamp=stamp;
    if(processing) processing->Poll();
    if(std::isfinite(hours)) lastHours=hours;
    globals::features::nativeIBL.SetCaptureContext(stamp);
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
        if(!Policy::CaptureDue(lastCaptureFrame,currentStamp.engineFrame)) return;
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
        const auto sky=!currentStamp.interior?globals::features::nativeIBL.GetSkySource(currentStamp):EnvironmentSource{};
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

json NativeCubemaps::Producer::GetDiagnostics()
{
    const auto publication=Get();
    return { {"nativeRadianceInputAdmitted",publication && publication->nativeRadianceInput},
        {"transitionalRasterInputAdmitted",publication && publication->rasterCompatibilityInput},
        {"lastCaptureFrame",lastCaptureFrame}, {"environmentRevision",publication ? publication->revision : 0},
        {"basePublished",publication && bool(publication->base)}, {"reflectionPublished",publication && bool(publication->reflection)},
        {"skyPublished",publication && bool(publication->sky)}, {"inputAllocationBytes",inputs ? inputs->AllocationBytes() : 0},
        {"inputFailure",inputs ? inputs->Failure() : ""},
        {"processing",processing ? processing->GetDiagnostics() : json::object()} };
}

void NativeCubemaps::Producer::InvalidateGeneration()
{
    if(processing) processing->Reset();
    if(inputs) inputs->Reset();
    globals::features::nativeIBL.SetCaptureContext({});
    currentStamp={}; ++epoch; lastCaptureFrame=UINT32_MAX; failureLogged=false;
}
void NativeCubemaps::Producer::Release()
{
    processing.reset(); inputs.reset();
    raw=nullptr; rawUAV=nullptr; inputShader=nullptr; inputConstants=nullptr;
    lastCaptureFrame=UINT32_MAX;
}

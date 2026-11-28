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
#include <DirectXTex.h>
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
    UINT oldFirst=0,oldCount=0;
    context->CSGetConstantBuffers1(0,1,oldCB.put(),&oldFirst,&oldCount);
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
    context->CSSetConstantBuffers1(0,1,&cb,&oldFirst,&oldCount);
    context->CSSetShader(oldShader.get(),nullptr,0);
    context->CSSetShaderResources(0,2,oldReads);
    for(auto* view:oldReads) if(view) view->Release();
    return valid;
}
DX12Native::EnvironmentSource NativeCubemaps::Producer::GetDefaultSource(const GraphicsBackend& backend)
{
    // Static asset decode only; no GPU readback and no legacy feature activation.
    if(!defaultAttempted && backend.d3d11Device) {
        defaultAttempted=true;
        try {
            const auto path=Util::PathHelpers::GetShadersPath()/"DynamicCubemaps"/"defaultcubemap.dds";
            DirectX::ScratchImage loadedImage,decoded,converted;
            DirectX::TexMetadata metadata{};
            DX::ThrowIfFailed(DirectX::LoadFromDDSFile(path.c_str(),DirectX::DDS_FLAGS_NONE,&metadata,loadedImage));
            if(!metadata.IsCubemap() || metadata.arraySize!=6 || metadata.width!=metadata.height ||
                !metadata.width || metadata.width>256)
                throw std::runtime_error("Unsupported default environment cube");
            const DirectX::ScratchImage* image=&loadedImage;
            if(DirectX::IsCompressed(metadata.format)) {
                DX::ThrowIfFailed(DirectX::Decompress(loadedImage.GetImages(),loadedImage.GetImageCount(),
                    metadata,DXGI_FORMAT_UNKNOWN,decoded));
                image=&decoded;
            }
            if(image->GetMetadata().format!=DXGI_FORMAT_R16G16B16A16_FLOAT) {
                DX::ThrowIfFailed(DirectX::Convert(image->GetImages(),image->GetImageCount(),image->GetMetadata(),
                    DXGI_FORMAT_R16G16B16A16_FLOAT,DirectX::TEX_FILTER_DEFAULT,DirectX::TEX_THRESHOLD_DEFAULT,converted));
                image=&converted;
            }
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width=desc.Height=UINT(metadata.width);desc.MipLevels=1;desc.ArraySize=6;
            desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.SampleDesc.Count=1;
            desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;
            std::array<D3D11_SUBRESOURCE_DATA,6> initial{};
            for(size_t face=0;face<initial.size();++face){
                const auto* pixels=image->GetImage(0,face,0);
                if(!pixels || !pixels->pixels || pixels->rowPitch>UINT32_MAX || pixels->slicePitch>UINT32_MAX)
                    throw std::runtime_error("Invalid default environment face");
                initial[face]={pixels->pixels,UINT(pixels->rowPitch),UINT(pixels->slicePitch)};
            }
            DX::ThrowIfFailed(backend.d3d11Device->CreateTexture2D(&desc,initial.data(),defaultCube.put()));
        } catch(const std::exception& e) {
            logger::warn("[NativeCubemaps] Default environment unavailable: {}",e.what());
        }
    }
    return defaultCube?DX12Native::EnvironmentSource{defaultCube.get(),1,currentStamp}:DX12Native::EnvironmentSource{};
}

void NativeCubemaps::Producer::Capture(const GraphicsBackend&, ID3D11ShaderResourceView*, uint32_t,
    std::shared_ptr<const DX12Native::FullNativeReconstructionFrame>)
{
    // Cube acquisition intentionally suspended; RT reflections use the scene directly.
}
std::shared_ptr<const DX12Native::EnvironmentFrame> NativeCubemaps::Producer::Get() const
{
    return nullptr;
}

void NativeCubemaps::Producer::SetupResources()
{
    // No cubemap resources in RT-only Full Native mode.
}

std::function<void()> NativeCubemaps::Producer::OnReflectionsRenderBegin()
{
    return nullptr;
}

DX12Native::EnvironmentSource NativeCubemaps::Producer::GetSkySource(const DX12Native::EnvironmentStamp&) const
{
    return {};
}

ID3D11ShaderResourceView* NativeCubemaps::Producer::GetRasterSkyView() const
{
    return nullptr;
}

void NativeCubemaps::Producer::BeginForward()
{
    EndForward();
}
void NativeCubemaps::Producer::EndForward()
{
    if(globals::state) globals::state->permutationData.ExtraShaderDescriptor &= ~uint32_t(State::ExtraShaderDescriptors::NativeForwardEnvironment);
    if(forward) forward->Unbind();
}

void NativeCubemaps::Producer::BeginWaterSSR()
{
    if (!loaded || globals::features::dx12Control.UsesLegacyReflectionCapture() || !globals::features::dx12Control.WantsMaterialPreparation()) return;
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
    if (!globals::features::dx12Control.WantsMaterialPreparation() ||
        !globals::features::dx12Control.NativeSSREnabled() || !globals::state ||
        !globals::game::graphicsState || !globals::state->currentShader ||
        globals::state->nativeFirstPersonDepth || globals::state->isMapMenuOpen ||
        (globals::state->permutationData.ExtraShaderDescriptor & uint32_t(State::ExtraShaderDescriptors::IsReflections))) return;
    const auto* shader = globals::state->currentShader;
    const auto frame = globals::game::graphicsState->GetFrameCount();
    const auto pluginFrame = globals::state->frameCount;
    // Use the same water stencil technique that writes the engine WaterMask.
    // This is recorded before the image-space reflection pass, without a GPU
    // query, readback, new render-pass hook or previous-frame visibility guess.
    if (shader->shaderType.get() == RE::BSShader::Type::Water) {
        const auto technique = (globals::state->currentPixelDescriptor >> 11u) & 0xFu;
        if (technique == static_cast<uint32_t>(SIE::ShaderCache::WaterShaderTechniques::Stencil)) {
            const auto* player = globals::game::player;
            const auto* cell = player ? player->GetParentCell() : nullptr;
            if (cell) waterDraw.Observe(frame,pluginFrame,cell->formID);
        }
        return;
    }
    if (waterScopeDepth != 1 || waterScopeInvalid ||
        shader->shaderType.get() != RE::BSShader::Type::ImageSpace ||
        std::string_view(static_cast<const RE::BSImagespaceShader&>(*shader).originalShaderName) != "ISReflectionsRayTracing") return;
    const auto* player = globals::game::player;
    const auto* cell = player ? player->GetParentCell() : nullptr;
    const auto cellID = cell ? cell->formID : 0u;
    const bool visibleWater = waterDraw.Matches(frame,pluginFrame,cellID);
    const bool cellEligible = cell && Policy::WaterCellEligible(cellID,cell->IsInteriorCell(),
        cell->cellFlags.any(RE::TESObjectCELL::Flag::kHasWater),visibleWater);
    waterFrameEligible = cellEligible && visibleWater;
    if (!waterFrameEligible) {
        if (!cellEligible) {
            ++dryCellSkips;
            // The destructor retires slots against all three GPU fences.
            // Dry interiors must not retain water-specific allocations.
            waterSSR.reset();
        } else {
            ++noWaterDrawSkips;
            if (waterSSR) waterSSR->ResetHistory();
        }
        return;
    }
    const auto world = globals::features::dx12NativeScene.GetWorldFrame(frame,pluginFrame);
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
    return { {"cubeCaptureSuspended",true}, {"nativeRadianceInputAdmitted",publication && publication->nativeRadianceInput},
        {"transitionalRasterInputAdmitted",publication && publication->rasterCompatibilityInput},
        {"lastCaptureFrame",lastCaptureFrame}, {"captureInterval",globals::features::dx12Control.NativeCubemapCaptureInterval()}, {"environmentRevision",publication ? publication->revision : 0},
        {"basePublished",publication && bool(publication->base)}, {"reflectionPublished",publication && bool(publication->reflection)},
        {"skyFacesValid",skyCapture.ValidFaces()}, {"skyRevision",skyRevision},
        {"skyAcquisitionFresh",bool(GetSkySource(currentStamp).texture)}, {"skyHidden",currentStamp.skyHidden},
        {"skyPublished",publication && bool(publication->sky)}, {"defaultCubeLoaded",bool(defaultCube)}, {"inputAllocationBytes",inputs ? inputs->AllocationBytes() : 0},
        {"inputFailure",inputs ? inputs->Failure() : ""},
        {"processing",processing ? processing->GetDiagnostics() : json::object()},
        {"waterAdmission",{{"frameEligible",waterFrameEligible},{"observedStencil",waterDraw.observed},
            {"observedEngineFrame",waterDraw.engineFrame},{"observedPluginFrame",waterDraw.pluginFrame},
            {"observedCell",waterDraw.cell},{"dryCellSkips",dryCellSkips},{"noWaterDrawSkips",noWaterDrawSkips}}},
        {"waterSSR",waterSSR ? waterSSR->Diagnostics() : json::object()},
        {"forwardEnvironment",forward ? forward->Diagnostics() : json::object()} };
}

void NativeCubemaps::Producer::InvalidateGeneration()
{
    EndForward();
    if(forward) forward->Reset();
    if(waterSSR) { waterSSR->End(); waterSSR->ResetHistory(); }
    waterScopeDepth=0; waterScopeInvalid=false;
    waterDraw={}; waterFrameEligible=false;
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
    defaultCube=nullptr; defaultAttempted=false;
    lastCaptureFrame=UINT32_MAX;
}

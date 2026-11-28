#include "NativeCubemaps.h"
#include "NativeCubemapPolicy.h"
#include "Features/DX12/DX12FullNativeReconstruction.h"
#include "Features/DX12/DX12NativeScene.h"
#include "Features/DX12/DX12Resources.h"

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
#include <cmath>
#include <DirectXTex.h>
#include <stdexcept>

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

void NativeCubemaps::Producer::OnOpaqueRasterReady()
{
    // Native Scene's existing opaque hook runs first. No legacy scene-color
    // capture, raster ABI change, or dependency on final-color application.
    if (!loaded || !globals::features::dx12Control.WantsMaterialPreparation() ||
        globals::features::dx12Control.UsesLegacyReflectionCapture() || !globals::state ||
        !globals::state->inWorld || globals::state->nativeFirstPersonDepth ||
        globals::state->isMapMenuOpen || globals::features::dx12Resources.IsLoading() ||
        !globals::game::graphicsState ||
        (globals::state->permutationData.ExtraShaderDescriptor & uint32_t(State::ExtraShaderDescriptors::IsReflections))) return;
    const auto* backend=globals::features::dx12NativeScene.GetNativeBackend();
    const auto grid=globals::features::dx12Resources.GetGrid();
    const uint32_t frame=globals::game::graphicsState->GetFrameCount();
    if(!backend || !grid || grid->requested.empty() ||
        !GraphicsBackend::CheckNativeDeviceHealth(backend->d3d12Device.get(),backend->d3d11Device.get())) return;
    bool changed=false;
    Prepare(grid->requested.front().scope,frame,changed);
    if(changed) globals::features::nativeIBL.InvalidateGeneration();
    Capture(*backend,nullptr,frame,globals::features::upscaling.nativeComposite.GetFullNativeReconstruction(frame));
    BeginForward();
}

void NativeCubemaps::Producer::Capture(const GraphicsBackend& backend, ID3D11ShaderResourceView*, uint32_t frame,
    std::shared_ptr<const DX12Native::FullNativeReconstructionFrame> native)
{
    using namespace DX12Native;
    if(!loaded || !globals::features::dx12Control.WantsMaterialPreparation() ||
        globals::features::dx12Control.UsesLegacyReflectionCapture() ||
        !currentStamp.scope || currentStamp.engineFrame != frame || !globals::state ||
        currentStamp.pluginFrame != globals::state->frameCount || !globals::game::renderer ||
        !GraphicsBackend::CheckNativeDeviceHealth(backend.d3d12Device.get(),backend.d3d11Device.get())) return;
    const auto world=globals::features::dx12NativeScene.GetWorldFrame(frame,currentStamp.pluginFrame);
    if(!world) return;
    try {
        if(processing) {
            processing->Advance(frame);
            if(processing->Pending()) return;
        }
        if(!Policy::CaptureDue(lastCaptureFrame,frame,globals::features::dx12Control.NativeCubemapCaptureInterval())) return;
        // Only direct diffuse/emission/transmission observations enter cube
        // history. Indirect or final composited color would create feedback.
        if(!native || native->frame!=frame || !native->material ||
            native->material->scope!=currentStamp.scope || !native->direct ||
            native->direct->material!=native->material || !native->rasterEvidence ||
            !native->rasterEvidence->tokenCount || !native->ownershipMask || !native->environmentRadiance)
            native.reset();
        std::array<float4,3> dalc{};
        if(!globals::state->GetAmbientSH(frame,currentStamp.pluginFrame,dalc)) return;
        if(!inputs) inputs=std::make_unique<NativeEnvironment>();
        const auto sky=GetSkySource(currentStamp);
        EnvironmentSource reflection{};
        if(sky.IsValidFor(currentStamp)) {
            const auto& target=globals::game::renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];
            reflection={Util::AsReal(target.texture),sky.revision,sky.stamp};
        }
        // DX11 supplies engine acquisition textures and the static DDS only.
        // Bootstrap from DALC/default/engine reflection before native samples
        // are available; never dispatch CaptureRaw's DX11 compute path.
        inputs->Capture(backend,currentStamp,{},reflection,sky,true,GetDefaultSource(backend));
        auto captured=inputs->Get();
        if(!captured) return;
        if(!processing) processing=std::make_unique<CubeProcessing>();
        if(processing->Dispatch(backend,captured,dalc,world->camera.CameraPosAdjust,native)) {
            lastCaptureFrame=frame;
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
    const auto frame=processing ? processing->Get() : nullptr;
    if(!frame || !frame->base || !frame->base->IsReadyFor(currentStamp) ||
        !Policy::CompletedGenerationFresh(frame->stamp.SameEnvironment(currentStamp),
            frame->stamp.engineFrame,currentStamp.engineFrame) ||
        !frame->producerFence || !frame->producerValue) return nullptr;
    const auto completed=frame->producerFence->GetCompletedValue();
    if(completed==UINT64_MAX || completed<frame->producerValue) return nullptr;
    const auto fresh=[&](const auto& cube) {
        return !cube || (cube->IsReadyFor(currentStamp) &&
            Policy::PublicationFresh(true,cube->stamp.engineFrame,currentStamp.engineFrame));
    };
    if(!fresh(frame->reflection) || !fresh(frame->sky)) {
        auto partial=std::make_shared<DX12Native::EnvironmentFrame>(*frame);
        if(!fresh(partial->reflection)) partial->reflection.reset();
        if(!fresh(partial->sky)) partial->sky.reset();
        return partial;
    }
    return frame;
}

void NativeCubemaps::Producer::SetupResources()
{
    if(loaded && globals::features::dx12Control.WantsMaterialPreparation() && globals::game::renderer) skyCapture.Setup();
}

std::function<void()> NativeCubemaps::Producer::OnReflectionsRenderBegin()
{
    using namespace DX12Native;
    if(!loaded || !globals::features::dx12Control.WantsMaterialPreparation() ||
        globals::features::dx12Control.UsesLegacyReflectionCapture()) return nullptr;
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

ID3D11ShaderResourceView* NativeCubemaps::Producer::GetRasterSkyView() const
{
    if (!loaded || !globals::features::dx12Control.WantsMaterialPreparation() ||
        !globals::game::graphicsState || !globals::state || skyCapture.IsCapturing() ||
        Util::IsInterior() || !globals::game::sky || globals::game::sky->flags.any(RE::Sky::Flags::kHideSky))
        return nullptr;
    auto requested = currentStamp;
    requested.engineFrame = globals::game::graphicsState->GetFrameCount();
    requested.pluginFrame = globals::state->frameCount;
    return GetSkySource(requested).texture ? skyCapture.View() : nullptr;
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
    // Sky acquisition is independent of the water SSR toggle and admission.
    skyCapture.OnDraw();
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
    return { {"cubeCaptureSuspended",false}, {"fallbackOnly",publication && !publication->nativeRadianceInput && !publication->rasterCompatibilityInput}, {"nativeRadianceInputAdmitted",publication && publication->nativeRadianceInput},
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
    defaultCube=nullptr; defaultAttempted=false;
    lastCaptureFrame=UINT32_MAX;
}

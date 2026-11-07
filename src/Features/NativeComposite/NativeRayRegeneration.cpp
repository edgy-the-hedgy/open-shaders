#include "NativeRayRegeneration.h"
#include "Globals.h"
#include "Utils/D3D.h"
#include "Utils/SehGuard.h"
#include "Features/Upscaling.h"
#include "Features/ReverseZ.h"
#include "Utils/FileSystem.h"
#include <filesystem>
#include <stdexcept>
#include <directx/d3dx12.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <vector>

namespace
{
    using RR = NativeRayRegeneration;

    std::vector<char> ReadBytecode(const char* path)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error(std::string("Missing RR shader: ") + path);
        auto size = input.tellg();
        if (size <= 0 || size > 16 * 1024 * 1024)
            throw std::runtime_error(std::string("Invalid RR shader size: ") + path);
        std::vector<char> binary(static_cast<size_t>(size));
        input.seekg(0);
        if (!input.read(binary.data(), size))
            throw std::runtime_error(std::string("Incomplete RR shader read: ") + path);
        return binary;
    }

    void MakeComputePipeline(ID3D12Device* device, const char* shaderPath, uint32_t srvs,
        uint32_t uavs, uint32_t constants,
        winrt::com_ptr<ID3D12RootSignature>& root,
        winrt::com_ptr<ID3D12PipelineState>& pipeline)
    {
        const auto bytes = ReadBytecode(shaderPath);
        CD3DX12_DESCRIPTOR_RANGE ranges[2];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, srvs, 0);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, uavs, 0);
        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsDescriptorTable(1, &ranges[0]);
        params[1].InitAsDescriptorTable(1, &ranges[1]);
        params[2].InitAsConstants(constants, 0);
        const CD3DX12_ROOT_SIGNATURE_DESC desc(3, params);
        winrt::com_ptr<ID3DBlob> serialized, errors;
        DX::ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.put(), errors.put()));
        winrt::com_ptr<ID3D12RootSignature> newRoot;
        DX::ThrowIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(newRoot.put())));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = newRoot.get();
        pso.CS = { bytes.data(), bytes.size() };
        winrt::com_ptr<ID3D12PipelineState> newPipeline;
        DX::ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(newPipeline.put())));
        root = std::move(newRoot);
        pipeline = std::move(newPipeline);
    }

    void CreateSRV(ID3D12Device* device, ID3D12Resource* resource,
        D3D12_CPU_DESCRIPTOR_HANDLE target, DXGI_FORMAT nullFormat = DXGI_FORMAT_R32_FLOAT)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = resource ? resource->GetDesc().Format : nullFormat;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource, &desc, target);
    }

    void CreateUAV(ID3D12Device* device, ID3D12Resource* resource,
        D3D12_CPU_DESCRIPTOR_HANDLE target)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = resource->GetDesc().Format;
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(resource, nullptr, &desc, target);
    }

    void Transition(std::vector<D3D12_RESOURCE_BARRIER>& out, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        if (resource && from != to)
            out.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource, from, to));
    }
}

bool NativeRayRegeneration::IsComplete(ID3D12Fence* fence, uint64_t value)
{
    if (!fence || !value) return true;
    const auto done = fence->GetCompletedValue();
    return done != UINT64_MAX && done >= value;
}

NativeRayRegeneration::~NativeRayRegeneration()
{
    RetireContexts();
    // Never destroy or unload AMD contexts still referenced by GPU work.
    bool canUnload = IsComplete(lastContextFence.get(), lastContextValue);
    for (const auto& old : retired)
        canUnload = canUnload && IsComplete(old.fence.get(), old.value);
    if (canUnload && runtimeApi.DestroyContext) {
        if (context) (void)runtimeApi.DestroyContext(&context, nullptr);
        for (auto& old : retired)
            if (old.context) (void)runtimeApi.DestroyContext(&old.context, nullptr);
    } else if (context || !retired.empty()) {
        canUnload = false;
    }
    if (canUnload) {
        if (denoiserModule) FreeLibrary(denoiserModule);
        if (loaderModule) FreeLibrary(loaderModule);
    }
}

void NativeRayRegeneration::RetireContexts()
{
    if (!runtimeApi.DestroyContext) return;
    for (auto it = retired.begin(); it != retired.end();) {
        if (IsComplete(it->fence.get(), it->value)) {
            if (it->context) (void)runtimeApi.DestroyContext(&it->context, nullptr);
            it = retired.erase(it);
        } else ++it;
    }
}

bool NativeRayRegeneration::EnsureRuntime()
{
    if (loaderModule && denoiserModule && runtimeApi.CreateContext &&
        runtimeApi.DestroyContext && runtimeApi.Dispatch) return true;
    // The denoiser is dynamically registered as an AMD provider; this remains
    // independent of whether Upscaling has loaded its own FFX runtime.
    const auto directory = (Util::PathHelpers::GetDataPath() /
        "Shaders" / "Upscaling" / "FidelityFX").lexically_normal();
    if (!directory.is_absolute()) { status = "RR runtime directory is not absolute"; return false; }
    constexpr DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS;
    static DLL_DIRECTORY_COOKIE registered = nullptr;
    if (!registered) {
        const auto kernel = GetModuleHandleW(L"kernel32.dll");
        using RegisterDir = DLL_DIRECTORY_COOKIE(WINAPI*)(PCWSTR);
        const auto fn = kernel ? reinterpret_cast<RegisterDir>(
            GetProcAddress(kernel, "AddDllDirectory")) : nullptr;
        if (fn) registered = fn(directory.c_str());
    }
    if (!registered) { status = "RR runtime DLL directory registration failed"; return false; }
    if (!loaderModule)
        loaderModule = LoadLibraryExW((directory / "amd_fidelityfx_loader_dx12.dll").c_str(), nullptr, flags);
    if (!loaderModule) { status = "AMD FidelityFX DX12 loader DLL missing"; return false; }
    if (!denoiserModule)
        denoiserModule = LoadLibraryExW((directory / "amd_fidelityfx_denoiser_dx12.dll").c_str(), nullptr, flags);
    if (!denoiserModule) { status = "AMD RR 1.2.1 denoiser DLL missing"; return false; }
    ffxLoadFunctions(&runtimeApi, loaderModule);
    if (!runtimeApi.CreateContext || !runtimeApi.DestroyContext || !runtimeApi.Dispatch) {
        status = "AMD FidelityFX runtime missing required API exports";
        return false;
    }
    return true;
}

bool NativeRayRegeneration::EnsureContext(ID3D12Device* device, uint32_t width, uint32_t height,
    uint32_t signalFlags, uint32_t checkerboardFlags)
{
    RetireContexts();
    if (!EnsureRuntime()) return false;
    if (sdkFailed) { status = "FSR RR provider failed this session; raw bypass"; return false; }
    if (context && (contextDevice != device || contextWidth != width ||
        contextHeight != height || contextFlags != signalFlags ||
        contextCheckerboard != checkerboardFlags)) {
        if (retired.size() >= 8) { status = "RR context retirement budget reached"; return false; }
        if (IsComplete(lastContextFence.get(), lastContextValue))
            (void)runtimeApi.DestroyContext(&context, nullptr);
        else {
            Retired old{};
            old.context = context;
            old.fence = lastContextFence;
            old.value = lastContextValue;
            retired.push_back(std::move(old));
            context = nullptr;
        }
        context = nullptr;
        lastRRFrameValid = false;
        previousValid = false;
    }
    if (context) return true;
    ffx::CreateContextDescDenoiser desc{};
    desc.version = FFX_DENOISER_VERSION;
    desc.maxRenderSize = { width, height };
    desc.signalFlags = signalFlags;
    desc.checkerboardSignalFlags = checkerboardFlags;
    ffx::CreateBackendDX12Desc backend{};
    backend.device = device;
    ffx::ReturnCode result = ffx::ReturnCode::Error;
    DWORD faultCode = 0;
    const bool safe = Util::SehGuarded([&] {
        desc.header.pNext = &backend.header;
        backend.header.pNext = nullptr;
        result = static_cast<ffx::ReturnCode>(
            runtimeApi.CreateContext(&context, &desc.header, nullptr));
    }, &faultCode);
    if (!safe || result != ffx::ReturnCode::Ok || !context) {
        status = "AMD FSR RR 1.2.1 context creation unsupported or failed; raw bypass";
        if (context) (void)runtimeApi.DestroyContext(&context, nullptr);
        context = nullptr;
        sdkFailed = true;
        return false;
    }
    contextDevice = device; contextWidth = width; contextHeight = height;
    contextFlags = signalFlags; contextCheckerboard = checkerboardFlags;
    lastRRFrameValid = false;
    status = "AMD FSR RR 1.2.1 context ready";
    return true;
}

bool NativeRayRegeneration::EnsureShaders(ID3D12Device* device)
{
    if (shaderDevice == device && prepareRoot && composeRoot && preparePso && composePso)
        return true;
    prepareRoot = nullptr; composeRoot = nullptr;
    preparePso = nullptr; composePso = nullptr;
    try {
        MakeComputePipeline(device, "Data/Shaders/NativeComposite/RRPrepare.dxil", 9, 5, 36,
            prepareRoot, preparePso);
        MakeComputePipeline(device, "Data/Shaders/NativeComposite/RRCompose.dxil", 5, 1, 12,
            composeRoot, composePso);
        shaderDevice = device;
        return true;
    } catch (const std::exception& e) {
        status = e.what();
        return false;
    }
}

uint64_t NativeRayRegeneration::EstimateBytes(ID3D12Device* device,
    uint32_t width, uint32_t height) const
{
    if (!device || !width || !height) return UINT64_MAX;
    uint64_t bytes = 2ull * 1024 * 1024;
    for (size_t i = 0; i < Resource::Count; ++i) {
        auto format = i == Resource::LinearDepth ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height,
            1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto n = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        if (n == UINT64_MAX || bytes > UINT64_MAX - n) return UINT64_MAX;
        bytes += n;
    }
    return bytes;
}

void NativeRayRegeneration::Allocate(ID3D12Device* device, Slot& slot,
    uint32_t width, uint32_t height)
{
    slot = {};
    slot.width = width; slot.height = height;
    slot.allocationBytes = EstimateBytes(device, width, height);
    if (slot.allocationBytes == UINT64_MAX)
        throw std::runtime_error("RR resource allocation size unsupported");
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    for (size_t i = 0; i < Resource::Count; ++i) {
        const auto fmt = i == Resource::LinearDepth ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(fmt, width, height,
            1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        DX::ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(slot.resources[i].put())));
    }
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    desc.NumDescriptors = 14; // RRPrepare: 9 SRVs and 5 UAVs
    DX::ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(slot.preparationDescriptors.put())));
    desc.NumDescriptors = 6; // RRCompose: 5 SRVs and 1 UAV
    DX::ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(slot.compositionDescriptors.put())));
}

NativeRayRegeneration::Output NativeRayRegeneration::Prepare(ID3D12Device* device,
    ID3D12GraphicsCommandList* commands, Slot& slot,
    const std::shared_ptr<const NativeLightingFeature::Contribution>& lighting,
    const std::shared_ptr<const DX12Native::RasterFrame>& raster,
    uint32_t frame, const Selection& selection)
{
    Output out{};
    if (!selection.enabled || !commands || !lighting || !raster || !slot.Ready() ||
        !raster->cameraValid || frame != lighting->frame || frame != raster->frame ||
        lighting->width != slot.width || lighting->height != slot.height ||
        raster->width < slot.width || raster->height < slot.height) {
        status = "RR preparation blocked: missing same-frame native signals, raster guides or camera";
        return out;
    }
    for (size_t i = 10; i < 16; ++i)
        if (!lighting->textures[i]) { status = "RR raw signals or material guides unavailable"; return out; }
    for (auto input : { DX12Native::RasterFrame::Input::Motion,
        DX12Native::RasterFrame::Input::NormalRoughness,
        DX12Native::RasterFrame::Input::Albedo,
        DX12Native::RasterFrame::Input::Reflectance })
        if (!raster->inputs[size_t(input)] || !raster->inputs[size_t(input)]->resource)
            { status = "RR raster geometry guide unavailable"; return out; }
    if (!raster->depth || !raster->depth->resource || !EnsureShaders(device)) return out;

    // Preparation reads only geometry/material guidance, not lit raster color.
    auto* linear = slot.resources[Resource::LinearDepth].get();
    const bool usePrevious = previousValid && previousDepth &&
        previousSceneRevision == lighting->sceneRevision &&
        previousFrame + 1u == frame && previousWidth == slot.width &&
        previousHeight == slot.height && previousDepth.get() != linear;
    const std::array<ID3D12Resource*, 9> sources{
        raster->depth->resource.get(),
        raster->inputs[size_t(DX12Native::RasterFrame::Input::Motion)]->resource.get(),
        raster->inputs[size_t(DX12Native::RasterFrame::Input::NormalRoughness)]->resource.get(),
        raster->inputs[size_t(DX12Native::RasterFrame::Input::Albedo)]->resource.get(),
        raster->inputs[size_t(DX12Native::RasterFrame::Input::Reflectance)]->resource.get(),
        lighting->textures[12].get(), lighting->textures[13].get(), lighting->textures[14].get(),
        usePrevious ? previousDepth.get() : nullptr
    };
    const auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = slot.preparationDescriptors->GetCPUDescriptorHandleForHeapStart();
    for (auto* resource : sources) {
        CreateSRV(device, resource, cpu);
        cpu.ptr += step;
    }
    for (size_t i = 0; i < 5; ++i) {
        CreateUAV(device, slot.resources[i].get(), cpu);
        cpu.ptr += step;
    }
    std::vector<D3D12_RESOURCE_BARRIER> begin;
    for (auto* resource : sources)
        Transition(begin, resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (size_t i = 0; i < 5; ++i)
        Transition(begin, slot.resources[i].get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commands->ResourceBarrier(UINT(begin.size()), begin.data());

    ID3D12DescriptorHeap* heaps[]{ slot.preparationDescriptors.get() };
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetPipelineState(preparePso.get());
    commands->SetComputeRootSignature(prepareRoot.get());
    auto gpu = slot.preparationDescriptors->GetGPUDescriptorHandleForHeapStart();
    commands->SetComputeRootDescriptorTable(0, gpu);
    gpu.ptr += 9ull * step;
    commands->SetComputeRootDescriptorTable(1, gpu);
    struct Constants {
        uint32_t width, height, history, reverseZ;
        DirectX::SimpleMath::Matrix inverseProjection, inverseView;
    };
    static_assert(sizeof(Constants) == 36 * sizeof(uint32_t));
    const auto& camera = raster->camera;
    const Constants constants{ slot.width, slot.height, usePrevious ? 1u : 0u,
        globals::features::reverseZ.IsActive() ? 1u : 0u, camera.CameraProjInverse, camera.CameraViewInverse };
    commands->SetComputeRoot32BitConstants(2, 36, &constants, 0);
    commands->Dispatch((slot.width + 7) / 8, (slot.height + 7) / 8, 1);

    std::vector<D3D12_RESOURCE_BARRIER> prepared;
    for (auto* resource : sources)
        Transition(prepared, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    for (size_t i = 0; i < 5; ++i)
        Transition(prepared, slot.resources[i].get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commands->ResourceBarrier(UINT(prepared.size()), prepared.data());

    out.ready = true; out.frame = frame; out.raster = raster; out.lighting = lighting;
    const uint32_t mode = lighting->traceDensityMode;
    out.bypassedTwoThirds = mode == 1u;
    if (mode == 1u) { ++rawBypasses; status = "RR bypassed: Two-thirds random sparse pattern is unsupported"; return out; }

    uint32_t flags = 0;
    if (selection.diffuse && (lighting->enabledLobes & 1u)) flags |= FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE;
    if (selection.specular && (lighting->enabledLobes & 2u)) flags |= FFX_DENOISER_SIGNAL_DIRECT_SPECULAR;
    if (!flags) { ++rawBypasses; status = "RR bypassed: no direct signals selected"; return out; }
    const uint32_t checkerboard = mode == 2u ? flags : 0u;
    if (!EnsureContext(device, slot.width, slot.height, flags, checkerboard)) {
        ++rawBypasses; return out;
    }

    std::vector<D3D12_RESOURCE_BARRIER> rrBegin;
    for (size_t i = 10; i <= 11; ++i)
        Transition(rrBegin, lighting->textures[i].get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (flags & FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE)
        Transition(rrBegin, slot.resources[Resource::DenoisedDiffuse].get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (flags & FFX_DENOISER_SIGNAL_DIRECT_SPECULAR)
        Transition(rrBegin, slot.resources[Resource::DenoisedSpecular].get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commands->ResourceBarrier(UINT(rrBegin.size()), rrBegin.data());

    ffx::DispatchDescDenoiser common{};
    common.commandList = commands;
    common.linearDepth = ffxApiGetResourceDX12(slot.resources[Resource::LinearDepth].get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    common.motionVectors = ffxApiGetResourceDX12(slot.resources[Resource::MotionDepth].get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    common.normals = ffxApiGetResourceDX12(slot.resources[Resource::Normals].get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    common.diffuseAlbedo = ffxApiGetResourceDX12(slot.resources[Resource::DiffuseAlbedo].get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    common.specularAlbedo = ffxApiGetResourceDX12(slot.resources[Resource::SpecularAlbedo].get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    common.motionVectorScale = { 1.0f, 1.0f, 1.0f };
    // GPU guides already express PreviousUV-CurrentUV; preserve SDK's
    // jitter convention in screen pixel units.
    common.jitterOffsets = { globals::features::upscaling.jitter.x, globals::features::upscaling.jitter.y };
    common.cameraPositionDelta = {
        camera.CameraPreviousPosAdjust.x - camera.CameraPosAdjust.x,
        camera.CameraPreviousPosAdjust.y - camera.CameraPosAdjust.y,
        camera.CameraPreviousPosAdjust.z - camera.CameraPosAdjust.z
    };
    static_assert(sizeof(common.view) == sizeof(camera.CameraView));
    std::memcpy(&common.view, &camera.CameraView, sizeof(common.view));
    std::memcpy(&common.projection, &camera.CameraProjUnjittered, sizeof(common.projection));
    common.linearDepthBounds = { -60000.0f, 60000.0f };
    common.renderSize = { slot.width, slot.height };
    common.frameIndex = frame;
    common.flags = !lastRRFrameValid || lastRRFrame + 1u != frame ||
        lastRRSceneRevision != lighting->sceneRevision ||
        !usePrevious ? FFX_DENOISER_DISPATCH_RESET : 0u;

    ffx::DispatchDescDenoiserDirectDiffuse diffuse{};
    ffx::DispatchDescDenoiserDirectSpecular specular{};
    const uint32_t origin = mode == 2u ? ((frame ^ 0x51a3f62bu) & 1u) : 0u;
    if (flags & FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE) {
        diffuse.signal.input = ffxApiGetResourceDX12(lighting->textures[10].get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        diffuse.signal.output = ffxApiGetResourceDX12(slot.resources[Resource::DenoisedDiffuse].get(),
            FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
        diffuse.signal.checkerboardOrigin = origin;
        common.header.pNext = &diffuse.header;
    }
    if (flags & FFX_DENOISER_SIGNAL_DIRECT_SPECULAR) {
        specular.signal.input = ffxApiGetResourceDX12(lighting->textures[11].get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        specular.signal.output = ffxApiGetResourceDX12(slot.resources[Resource::DenoisedSpecular].get(),
            FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
        specular.signal.checkerboardOrigin = origin;
        if (flags & FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE)
            diffuse.header.pNext = &specular.header;
        else
            common.header.pNext = &specular.header;
    }
    // Call the pinned SDK only after all raw inputs are fully recorded and
    // without exposing Scene color or any raster lighting to the API.
    ffxReturnCode_t result = FFX_API_RETURN_ERROR;
    DWORD faultCode = 0;
    const bool safe = Util::SehGuarded([&] {
        result = runtimeApi.Dispatch(&context, &common.header);
    }, &faultCode);
    if (safe && result == FFX_API_RETURN_OK) {
        out.dispatched = true;
        out.diffuseDenoised = (flags & FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE) != 0u;
        out.specularDenoised = (flags & FFX_DENOISER_SIGNAL_DIRECT_SPECULAR) != 0u;
        ++rrDispatches;
        status = mode == 2u ? "RR submitted: checkerboard Half" : "RR submitted: Full";
    } else {
        status = safe ? "FSR RR dispatch rejected; raw bypass, provider quarantined" :
            "FSR RR provider access fault; raw bypass, provider quarantined";
        sdkFailed = true;
        ++rawBypasses;
    }

    std::vector<D3D12_RESOURCE_BARRIER> rrEnd;
    for (size_t i = 10; i <= 11; ++i)
        Transition(rrEnd, lighting->textures[i].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (flags & FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE)
        Transition(rrEnd, slot.resources[Resource::DenoisedDiffuse].get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (flags & FFX_DENOISER_SIGNAL_DIRECT_SPECULAR)
        Transition(rrEnd, slot.resources[Resource::DenoisedSpecular].get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    commands->ResourceBarrier(UINT(rrEnd.size()), rrEnd.data());
    return out;
}

bool NativeRayRegeneration::Compose(ID3D12Device* device,
    ID3D12GraphicsCommandList* commands, Slot& slot,
    ID3D12Resource* scene, DXGI_FORMAT sceneFormat,
    const Output& output, float strength, float diffuseContribution,
    float specularContribution, bool legacyTransfer)
{
    if (!output.ready || !output.lighting || !scene || !EnsureShaders(device))
        return false;
    const auto& lighting = output.lighting;
    const std::array<ID3D12Resource*, 5> sources{
        output.diffuseDenoised ? slot.resources[Resource::DenoisedDiffuse].get() : lighting->textures[10].get(),
        output.specularDenoised ? slot.resources[Resource::DenoisedSpecular].get() : lighting->textures[11].get(),
        slot.resources[Resource::DiffuseAlbedo].get(),
        slot.resources[Resource::SpecularAlbedo].get(),
        lighting->textures[15].get()
    };
    const auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = slot.compositionDescriptors->GetCPUDescriptorHandleForHeapStart();
    for (auto* src : sources) {
        CreateSRV(device, src, cpu);
        cpu.ptr += step;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = sceneFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(scene, nullptr, &uav, cpu);

    std::vector<D3D12_RESOURCE_BARRIER> begin;
    for (size_t i = 0; i < sources.size(); ++i)
        Transition(begin, sources[i], i == 2 || i == 3 ?
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!begin.empty()) commands->ResourceBarrier(UINT(begin.size()), begin.data());
    ID3D12DescriptorHeap* heaps[]{ slot.compositionDescriptors.get() };
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetPipelineState(composePso.get());
    commands->SetComputeRootSignature(composeRoot.get());
    auto gpu = slot.compositionDescriptors->GetGPUDescriptorHandleForHeapStart();
    commands->SetComputeRootDescriptorTable(0, gpu);
    gpu.ptr += 5ull * step;
    commands->SetComputeRootDescriptorTable(1, gpu);
    struct Constants {
        uint32_t width, height; float strength; uint32_t legacy;
        uint32_t diffuse, specular;
        float diffuseContribution, specularContribution;
        uint32_t emission, pad0, pad1, pad2;
    };
    static_assert(sizeof(Constants) == 12 * sizeof(uint32_t));
    const Constants constants{ slot.width, slot.height, strength, legacyTransfer ? 1u : 0u,
        (lighting->enabledLobes & 1u) ? 1u : 0u,
        (lighting->enabledLobes & 2u) ? 1u : 0u,
        diffuseContribution, specularContribution, 1u, 0u, 0u, 0u };
    commands->SetComputeRoot32BitConstants(2, 12, &constants, 0);
    commands->Dispatch((slot.width + 7) / 8, (slot.height + 7) / 8, 1);
    std::vector<D3D12_RESOURCE_BARRIER> end;
    for (auto* source : sources)
        Transition(end, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    // SDK depth, motion, and normal guides were left compute-readable. Return
    // every guide to COMMON before this fenced slot can be recycled.
    for (size_t i : { size_t(Resource::LinearDepth), size_t(Resource::MotionDepth), size_t(Resource::Normals) })
        Transition(end, slot.resources[i].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (!end.empty()) commands->ResourceBarrier(UINT(end.size()), end.data());
    ++compositePasses;
    return true;
}

void NativeRayRegeneration::OnSubmitted(const Slot& slot, ID3D12Fence* fence,
    uint64_t value, const Output& output)
{
    if (!output.ready || !fence || !value) return;
    previousDepth = slot.resources[Resource::LinearDepth];
    previousFrame = output.frame; previousWidth = slot.width; previousHeight = slot.height;
    previousValid = true;
    previousSceneRevision = output.lighting->sceneRevision;
    if (output.dispatched) {
        lastContextFence.copy_from(fence);
        lastContextValue = value;
        lastRRFrame = output.frame;
        lastRRSceneRevision = output.lighting->sceneRevision;
        lastRRFrameValid = true;
    } else lastRRFrameValid = false;
}

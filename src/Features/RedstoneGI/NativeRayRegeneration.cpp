#include "RuntimeDenoiser.h"
#include "Utils/RayRegenerationSettings.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Util.h"
#include <directx/d3dx12.h>

namespace
{
    constexpr std::array<uint64_t, 7> kNativeKeys{
        FFX_API_CONFIGURE_DENOISER_KEY_CROSS_BILATERAL_NORMAL_STRENGTH,
        FFX_API_CONFIGURE_DENOISER_KEY_STABILITY_BIAS,
        FFX_API_CONFIGURE_DENOISER_KEY_MAX_RADIANCE,
        FFX_API_CONFIGURE_DENOISER_KEY_RADIANCE_CLIP_STD_K,
        FFX_API_CONFIGURE_DENOISER_KEY_GAUSSIAN_KERNEL_RELAXATION,
        FFX_API_CONFIGURE_DENOISER_KEY_DISOCCLUSION_THRESHOLD,
        FFX_API_CONFIGURE_DENOISER_KEY_DEBUG_VIEW_LINEAR_DEPTH_BOUNDS };
    constexpr std::array kNativeKeyNames{ "normalStrength", "stabilityBias", "maxRadiance",
        "radianceClipStdK", "gaussianRelaxation", "disocclusionThreshold", "debugDepthMin", "debugDepthMax" };

    ffxReturnCode_t RecordProtected(const ffxFunctions& api, ffxContext* context,
        const ffxDispatchDescHeader* desc, bool& faulted)
    {
        faulted = false;
        __try { return api.Dispatch(context, desc); }
        __except (EXCEPTION_EXECUTE_HANDLER) { faulted = true; return FFX_API_RETURN_ERROR; }
    }

    FfxApiMatrix4x4 NativeMatrix(const Matrix& matrix)
    {
        const auto transposed = matrix.Transpose();
        FfxApiMatrix4x4 result{};
        static_assert(sizeof(result) == sizeof(transposed));
        std::memcpy(&result, &transposed, sizeof(result));
        return result;
    }
}

struct RedstoneDenoiser::NativeState
{
    struct Signal
    {
        ffxContext context = nullptr;
        std::array<uint32_t, 2> size{}, nativeSize{};
        uint32_t type = 0, lastFrame = UINT32_MAX;
        bool checkerboard = false, configured = false, faulted = false, recording = false;
        uint64_t fence = 0, configuration = UINT64_MAX, requests = 0, records = 0, bytes = 0;
        uint64_t historyRevision = UINT64_MAX;
        std::array<float, 8> defaults{};
        bool defaultsReady = false;
        std::string failure;
    };
    struct Retired { ffxContext context; uint64_t fence; uint64_t bytes; };
    std::array<Signal, 4> signals;
    std::vector<Retired> retired;
    winrt::com_ptr<ID3D12Fence> fence;
    uint64_t submitted = 0;

    void Poll(const ffxFunctions& api)
    {
        const auto done = fence ? fence->GetCompletedValue() : 0;
        std::erase_if(retired, [&](Retired& value) {
            if (value.fence > done && done != UINT64_MAX) return false;
            return api.DestroyContext(&value.context, nullptr) == FFX_API_RETURN_OK;
        });
    }

    bool Configure(Signal& signal, const ffxFunctions& api)
    {
        const auto& selected = RayRegeneration::GetConfiguration();
        if (signal.configured && signal.configuration == selected.revision) return true;
        if (!signal.defaultsReady) {
            for (size_t i = 0; i < kNativeKeys.size(); ++i) {
                FfxApiFloatBounds bounds{};
                ffxQueryDescDenoiserGetDefaultKeyValue query{};
                query.header.type = FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE;
                query.key = kNativeKeys[i]; query.count = 1;
                query.data = i < 6 ? static_cast<void*>(&signal.defaults[i]) : static_cast<void*>(&bounds);
                if (api.Query(&signal.context, &query.header) != FFX_API_RETURN_OK) {
                    signal.failure = std::format("SDK default query failed for key {}", query.key); return false;
                }
                if (i == 6) { signal.defaults[6] = bounds.min; signal.defaults[7] = bounds.max; }
            }
            if (!std::ranges::all_of(signal.defaults, [](float value) { return std::isfinite(value); })) {
                signal.failure = "SDK returned nonfinite defaults"; return false;
            }
            signal.defaultsReady = true;
        }
        std::array<float, 8> values = signal.defaults;
        for (size_t i = 0; i < values.size(); ++i) {
            const auto found = selected.overrides.find(kNativeKeyNames[i]);
            if (found != selected.overrides.end() && !found->is_null()) values[i] = found->get<float>();
        }
        for (size_t i = 0; i < kNativeKeys.size(); ++i) {
            const FfxApiFloatBounds bounds{ values[6], values[7] };
            ffxConfigureDescDenoiserKeyValue configure{};
            configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE;
            configure.key = kNativeKeys[i]; configure.count = 1;
            configure.data = i < 6 ? static_cast<const void*>(&values[i]) : static_cast<const void*>(&bounds);
            if (api.Configure(&signal.context, &configure.header) != FFX_API_RETURN_OK) {
                signal.configured = false;
                signal.failure = std::format("SDK configuration rejected key {} at revision {}", configure.key, selected.revision);
                return false;
            }
        }
        signal.configuration = selected.revision;
        signal.configured = true;
        signal.lastFrame = UINT32_MAX;
        return true;
    }
};

bool RedstoneDenoiser::InitializeNative(const GraphicsBackend& graphics)
{
    if (nativeState) { nativeState->Poll(denoiserFunctions); return backend.d3d12Device == graphics.d3d12Device; }
    if (!graphics.d3d12Device || !graphics.commandQueue) return false;
    const auto directory = (Util::PathHelpers::GetDataPath() / "Shaders" / "Upscaling" / "FidelityFX").lexically_normal();
    if (!directory.is_absolute()) return false;
    backend = graphics; denoiserFunctions = backend.fidelityFX;
    if (!denoiserFunctions.CreateContext || !denoiserFunctions.DestroyContext || !denoiserFunctions.Dispatch ||
        !denoiserFunctions.Configure || !denoiserFunctions.Query) return false;
    if (!denoiserModule) denoiserModule = LoadLibraryExW((directory / "amd_fidelityfx_denoiser_dx12.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!denoiserModule) return false;
    auto state = std::make_shared<NativeState>();
    DX::ThrowIfFailed(backend.d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(state->fence.put())));
    state->fence->SetName(L"RayRegeneration::NativeContextRetirement");
    nativeState = std::move(state);
    return true;
}

void RedstoneDenoiser::RetireNative(uint32_t mask)
{
    if (!nativeState) return;
    for (uint32_t i = 0; i < 4; ++i) {
        auto& signal = nativeState->signals[i];
        if (!(mask & (1u << i)) || !signal.context) continue;
        nativeState->retired.push_back({ signal.context, signal.fence, signal.bytes });
        signal.bytes = 0;
        signal.context = nullptr; signal.configured = false; signal.defaultsReady = false;
        signal.lastFrame = UINT32_MAX; signal.faulted = false;
    }
    nativeState->Poll(denoiserFunctions);
}

void RedstoneDenoiser::NotifyNativeSubmitted()
{
    if (!nativeState) return;
    const auto value = ++nativeState->submitted;
    const bool signalled = SUCCEEDED(backend.commandQueue->Signal(nativeState->fence.get(), value));
    for (auto& signal : nativeState->signals)
        if (signal.recording) {
            signal.fence = signalled ? value : UINT64_MAX;
            signal.recording = false;
            if (!signalled) { signal.faulted = true; signal.failure = "Native RR submission fence failed"; }
        }
}

void RedstoneDenoiser::CancelNativeRecording()
{
    if (!nativeState) return;
    for (auto& signal : nativeState->signals) if (signal.recording) {
        signal.lastFrame = UINT32_MAX; signal.recording = false;
    }
}

bool RedstoneDenoiser::DisposeNative()
{
    if (!nativeState) return true;
    auto preserve = [&] { (void)new std::shared_ptr<NativeState>(std::move(nativeState)); return false; };
    RetireNative(15u);
    if (!nativeState->retired.empty()) {
        winrt::handle event{ CreateEventW(nullptr, FALSE, FALSE, nullptr) };
        if (!event || FAILED(nativeState->fence->SetEventOnCompletion(nativeState->submitted, event.get())) ||
            WaitForSingleObject(event.get(), SharedFence::kFenceTimeoutMs) != WAIT_OBJECT_0) return preserve();
        nativeState->Poll(denoiserFunctions);
    }
    if (!nativeState->retired.empty()) return preserve();
    nativeState.reset();
    return true;
}

bool RedstoneDenoiser::RecordNativeSignal(ID3D12GraphicsCommandList* list, const NativeResources& resources, uint32_t index)
{
    CS_GPU_PASS("RayRegeneration::NativeSignalDispatch");
    if (!nativeState || !list || index >= 4) return false;
    auto& signal = nativeState->signals[index];
    ++signal.requests;
    const auto extent = resources.logicalSize;
    const auto type = index == 0 ? (resources.direct ? FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE : FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE) :
        index == 1 ? (resources.direct ? FFX_DENOISER_SIGNAL_DIRECT_SPECULAR : FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR) :
        index == 2 ? FFX_DENOISER_SIGNAL_AMBIENT_OCCLUSION : FFX_DENOISER_SIGNAL_SPECULAR_OCCLUSION;
    if (!extent[0] || !extent[1] || !resources.input || !resources.output ||
        std::ranges::any_of(resources.guides, [](auto* resource) { return !resource; })) {
        signal.failure = "Missing native signal, guide or extent"; return false;
    }
    const auto inputDesc = resources.input->GetDesc(), outputDesc = resources.output->GetDesc();
    const auto width = resources.checkerboard ? extent[0] / 2u : extent[0];
    const auto format = index < 2 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8_UNORM;
    if ((resources.checkerboard && (extent[0] & 1u)) || inputDesc.Width != width || inputDesc.Height != extent[1] ||
        outputDesc.Width != extent[0] || outputDesc.Height != extent[1] || inputDesc.Format != format || outputDesc.Format != format) {
        signal.failure = "Native signal format or sampling extent mismatch"; return false;
    }
    for (uint32_t i = 0; i < resources.guides.size(); ++i) {
        const auto desc = resources.guides[i]->GetDesc();
        if (desc.Width != extent[0] || desc.Height != extent[1] || desc.Format != (i ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32_FLOAT)) {
            signal.failure = "Native guide format or logical extent mismatch"; return false;
        }
    }
    if (signal.context && (signal.size != extent || signal.type != uint32_t(type) || signal.checkerboard != resources.checkerboard))
        RetireNative(1u << index);
    if (signal.faulted) { signal.failure = "SDK signal context quarantined after provider exception"; return false; }
    if (signal.context && GetNativeResidentBytes() > resources.contextBudgetBytes) {
        signal.failure = "Retained SDK allocations exceed the selected context allowance";
        RetireNative(1u << index); return false;
    }
    if (!signal.context) {
        FfxApiEffectMemoryUsage memory{};
        ffxQueryDescDenoiserGetGPUMemoryUsage query{};
        query.header.type = FFX_API_QUERY_DESC_TYPE_DENOISER_GPU_MEMORY_USAGE;
        query.device = backend.d3d12Device.get(); query.maxRenderSize = { extent[0], extent[1] };
        query.signalFlags = uint32_t(type); query.checkerboardSignalFlags = resources.checkerboard ? uint32_t(type) : 0u;
        query.gpuMemoryUsage = &memory;
        if (denoiserFunctions.Query(nullptr, &query.header) != FFX_API_RETURN_OK) {
            signal.failure = "SDK memory requirement query failed; raw signal retained"; return false;
        }
        const auto retained = GetNativeResidentBytes();
        if (retained > resources.contextBudgetBytes || memory.totalUsageInBytes > resources.contextBudgetBytes - retained) {
            signal.failure = std::format("SDK signal needs {} bytes with {} retained; context budget is {}", memory.totalUsageInBytes, retained, resources.contextBudgetBytes);
            return false;
        }
        ffxCreateContextDescDenoiser create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
        create.version = FFX_DENOISER_VERSION; create.maxRenderSize = { extent[0], extent[1] };
        create.signalFlags = uint32_t(type); create.checkerboardSignalFlags = resources.checkerboard ? uint32_t(type) : 0u;
        ffx::CreateBackendDX12Desc device{}; device.device = backend.d3d12Device.get();
        create.header.pNext = &device.header;
        if (denoiserFunctions.CreateContext(&signal.context, &create.header, nullptr) != FFX_API_RETURN_OK || !signal.context) {
            if (signal.context) RetireNative(1u << index);
            signal.failure = "SDK signal context creation failed"; return false;
        }
        signal.size = extent; signal.type = uint32_t(type); signal.checkerboard = resources.checkerboard;
        signal.bytes = memory.totalUsageInBytes;
        signal.fence = 0; signal.recording = false;
        if (denoiserFunctions.Query(&signal.context, &query.header) == FFX_API_RETURN_OK) signal.bytes = memory.totalUsageInBytes;
        if (GetNativeResidentBytes() > resources.contextBudgetBytes) {
            signal.failure = "Actual SDK allocation exceeds the signal's memory allowance";
            RetireNative(1u << index); return false;
        }
    }
    if (!nativeState->Configure(signal, denoiserFunctions)) return false;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    for (auto* guide : resources.guides) barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(guide,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resources.input,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resources.output,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    list->ResourceBarrier(UINT(barriers.size()), barriers.data());
    ffxDispatchDescDenoiser common{};
    common.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
    common.commandList = list;
    common.linearDepth = ffxApiGetResourceDX12(resources.guides[0], FFX_API_RESOURCE_STATE_COMPUTE_READ);
    common.motionVectors = ffxApiGetResourceDX12(resources.guides[1], FFX_API_RESOURCE_STATE_COMPUTE_READ);
    common.normals = ffxApiGetResourceDX12(resources.guides[2], FFX_API_RESOURCE_STATE_COMPUTE_READ);
    common.diffuseAlbedo = ffxApiGetResourceDX12(resources.guides[3], FFX_API_RESOURCE_STATE_COMPUTE_READ);
    common.specularAlbedo = ffxApiGetResourceDX12(resources.guides[4], FFX_API_RESOURCE_STATE_COMPUTE_READ);
    common.motionVectorScale = { 1, 1, 1 };
    const auto currentBackend = Feature::FindGraphicsBackend();
    const auto jitter = currentBackend ? currentBackend->jitter : backend.jitter;
    common.jitterOffsets = { -jitter.x * float(extent[0]) / float(resources.nativeSize[0]),
        -jitter.y * float(extent[1]) / float(resources.nativeSize[1]) };
    const auto& frame = globals::game::frameBufferCached;
    const auto& current = frame.GetCameraPosAdjust(); const auto& previous = frame.GetCameraPreviousPosAdjust();
    common.cameraPositionDelta = { previous.x - current.x, previous.y - current.y, previous.z - current.z };
    common.view = NativeMatrix(frame.GetCameraView()); common.projection = NativeMatrix(frame.GetCameraProjUnjittered());
    common.linearDepthBounds = { std::max(0.001f, *globals::game::cameraNear), *globals::game::cameraFar };
    common.renderSize = { extent[0], extent[1] }; common.frameIndex = resources.frame;
    common.flags = FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO;
    if (resources.reset || signal.lastFrame + 1u != resources.frame || signal.lastFrame == UINT32_MAX || signal.nativeSize != resources.nativeSize || signal.historyRevision != resources.historyRevision)
        common.flags |= FFX_DENOISER_DISPATCH_RESET;
    struct SignalDesc { ffxDispatchDescHeader header; FfxApiDenoiserSignal signal; } desc{};
    static_assert(sizeof(SignalDesc) == sizeof(ffxDispatchDescDenoiserDirectDiffuse));
    static_assert(sizeof(SignalDesc) == sizeof(ffxDispatchDescDenoiserIndirectSpecular));
    constexpr std::array<uint64_t, 4> indirectTypes{ FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE,
        FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR, FFX_API_DISPATCH_DESC_TYPE_DENOISER_AMBIENT_OCCLUSION,
        FFX_API_DISPATCH_DESC_TYPE_DENOISER_SPECULAR_OCCLUSION };
    desc.header.type = resources.direct && index < 2 ? (index == 0 ? FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE :
        FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR) : indirectTypes[index];
    desc.signal.input = ffxApiGetResourceDX12(resources.input, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.signal.output = ffxApiGetResourceDX12(resources.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
    desc.signal.checkerboardOrigin = resources.checkerboard ? resources.origin : 0u;
    common.header.pNext = &desc.header;
    const auto result = RecordProtected(denoiserFunctions, &signal.context, &common.header, signal.faulted);
    signal.recording = true;
    for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(UINT(barriers.size()), barriers.data());
    if (result != FFX_API_RETURN_OK || signal.faulted) {
        signal.lastFrame = UINT32_MAX;
        signal.failure = std::format("SDK signal dispatch failed ({})", result); return false;
    }
    signal.lastFrame = resources.frame; signal.nativeSize = resources.nativeSize; signal.historyRevision = resources.historyRevision;
    signal.failure.clear(); ++signal.records;
    return true;
}

json RedstoneDenoiser::GetNativeDiagnostics() const
{
    json result = json::array();
    if (!nativeState) return result;
    for (const auto& signal : nativeState->signals) result.push_back({
        { "initialized", signal.context != nullptr }, { "configured", signal.configured },
        { "requests", signal.requests }, { "dispatchesRecorded", signal.records },
        { "logicalSize", signal.size }, { "checkerboard", signal.checkerboard },
        { "lastRecordedFrame", signal.lastFrame }, { "failure", signal.failure },
        { "gpuBytes", signal.bytes },
        { "configurationRevision", signal.configuration },
        { "defaults", signal.defaultsReady ? json(signal.defaults) : json(nullptr) } });
    return result;
}

uint64_t RedstoneDenoiser::GetNativeResidentBytes() const
{
    uint64_t result = 0;
    if (!nativeState) return result;
    for (const auto& signal : nativeState->signals) result += signal.bytes;
    for (const auto& retired : nativeState->retired) result += retired.bytes;
    return result;
}

std::optional<std::array<float, 8>> RedstoneDenoiser::NativeDefaultsForUI() const
{
    if (nativeState) for (const auto& signal : nativeState->signals) if (signal.defaultsReady) return signal.defaults;
    return std::nullopt;
}

#pragma once

#include "Features/NativeLighting/NativeLighting.h"
#include "Features/DX12NativeScene/RasterCapture.h"
#include <FidelityFX/api/include/dx12/ffx_api_dx12.hpp>
#include <FidelityFX/api/include/ffx_api_loader.h>
#include <FidelityFX/denoisers/include/ffx_denoiser.hpp>
#include <array>
#include <memory>
#include <string>
#include <vector>

// Native Composite-owned FSR Ray Regeneration. GPU-only input/output;
// no DX11 raster COLOR is available to this component.
class NativeRayRegeneration
{
public:
    struct Selection
    {
        bool enabled = false;
        bool diffuse = true;
        bool specular = true;
    };

    enum Resource : size_t
    {
        LinearDepth, MotionDepth, Normals, DiffuseAlbedo, SpecularAlbedo,
        DenoisedDiffuse, DenoisedSpecular, Count
    };

    struct Slot
    {
        std::array<winrt::com_ptr<ID3D12Resource>, Resource::Count> resources;
        winrt::com_ptr<ID3D12DescriptorHeap> preparationDescriptors, compositionDescriptors;
        uint32_t width = 0, height = 0;
        uint64_t allocationBytes = 0;
        bool Ready() const { return resources[LinearDepth] && preparationDescriptors && compositionDescriptors; }
    };

    struct Output
    {
        bool ready = false;            // Guidance and source signals are available.
        bool dispatched = false;       // SDK denoiser actually recorded commands.
        bool diffuseDenoised = false;
        bool specularDenoised = false;
        bool bypassedTwoThirds = false;
        uint32_t frame = 0;
        std::shared_ptr<const DX12Native::RasterFrame> raster;
        std::shared_ptr<const NativeLightingFeature::Contribution> lighting;
    };

    ~NativeRayRegeneration();

    uint64_t EstimateBytes(ID3D12Device* device, uint32_t width, uint32_t height) const;
    void Allocate(ID3D12Device* device, Slot& slot, uint32_t width, uint32_t height);
    // Record RRPrepare and the AMD SDK call. Does NOT access the raster image.
    Output Prepare(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
        Slot& slot, const std::shared_ptr<const NativeLightingFeature::Contribution>& lighting,
        const std::shared_ptr<const DX12Native::RasterFrame>& raster,
        uint32_t frame, const Selection& selection);
    // Only this final stage reads the already-lit raster Scene UAV.
    bool Compose(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
        Slot& slot, ID3D12Resource* scene, DXGI_FORMAT sceneFormat,
        const Output& output, float strength, float diffuseContribution,
        float specularContribution, bool legacyTransfer);
    // Called only after the complete Native Composite command list is enqueued.
    void OnSubmitted(const Slot& slot, ID3D12Fence* fence, uint64_t value, const Output& output);
    const std::string& Status() const { return status; }
    uint64_t RRDispatches() const { return rrDispatches; }
    uint64_t RawBypasses() const { return rawBypasses; }
    uint64_t CompositePasses() const { return compositePasses; }

private:
    struct Retired
    {
        ffx::Context context{};
        winrt::com_ptr<ID3D12Fence> fence;
        uint64_t value = 0;
    };
    ffx::Context context{};
    std::vector<Retired> retired;
    ID3D12Device* contextDevice = nullptr;
    uint32_t contextWidth = 0, contextHeight = 0, contextFlags = 0, contextCheckerboard = 0;
    winrt::com_ptr<ID3D12Fence> lastContextFence;
    uint64_t lastContextValue = 0;
    uint32_t lastRRFrame = 0;
    uint64_t lastRRSceneRevision = 0;
    bool lastRRFrameValid = false;
    winrt::com_ptr<ID3D12Resource> previousDepth;
    uint32_t previousFrame = 0, previousWidth = 0, previousHeight = 0;
    uint64_t previousSceneRevision = 0;
    bool previousValid = false;
    winrt::com_ptr<ID3D12RootSignature> prepareRoot, composeRoot;
    winrt::com_ptr<ID3D12PipelineState> preparePso, composePso;
    ID3D12Device* shaderDevice = nullptr;
    // Loading and SDK calls are owned exclusively by Native Composite.
    HMODULE loaderModule = nullptr, denoiserModule = nullptr;
    ffxFunctions runtimeApi{};
    bool sdkFailed = false;
    uint64_t rrDispatches = 0, rawBypasses = 0, compositePasses = 0;
    std::string status = "FSR RR disabled";

    bool EnsureRuntime();
    bool EnsureShaders(ID3D12Device* device);
    bool EnsureContext(ID3D12Device* device, uint32_t width, uint32_t height,
        uint32_t signalFlags, uint32_t checkerboardFlags);
    void RetireContexts();
    static bool IsComplete(ID3D12Fence* fence, uint64_t value);
};

 
// Dedicated checkerboard Direct Diffuse context for independent native shadows.
// Native Lighting is intentionally not an input or a precondition.
class NativeShadowRayRegeneration
{
public:
    enum Resource : size_t {
        LinearDepth, MotionDepth, Normals, DiffuseAlbedo, PackedSignal, DenoisedSignal, Count
    };
    struct Slot {
        std::array<winrt::com_ptr<ID3D12Resource>, Resource::Count> resources;
        winrt::com_ptr<ID3D12DescriptorHeap> guideDescriptors, packingDescriptors;
        uint32_t width = 0, height = 0;
        uint64_t allocationBytes = 0;
        bool Ready() const {
            return resources[LinearDepth] && resources[DenoisedSignal] &&
                guideDescriptors && packingDescriptors;
        }
    };
    struct Output {
        bool dispatched = false;
        uint32_t frame = 0;
        uint64_t sceneRevision = 0;
    };

    ~NativeShadowRayRegeneration();
    uint64_t EstimateBytes(ID3D12Device* device, uint32_t width, uint32_t height) const;
    void Allocate(ID3D12Device* device, Slot& slot, uint32_t width, uint32_t height);
    Output Prepare(ID3D12Device* device, ID3D12GraphicsCommandList* commands,
        Slot& slot, const std::shared_ptr<const DX12Native::ShadowFrame>& shadow,
        const std::shared_ptr<const DX12Native::RasterFrame>& raster, uint32_t frame);
    void OnSubmitted(const Slot& slot, ID3D12Fence* fence, uint64_t value, const Output& output);
    const std::string& Status() const { return status; }
    uint64_t Dispatches() const { return dispatches; }
    uint64_t Bypasses() const { return bypasses; }
    bool Failed() const { return sdkFailed; }
private:
    struct Retired {
        ffx::Context context{};
        winrt::com_ptr<ID3D12Fence> fence;
        uint64_t value = 0;
    };
    bool EnsureRuntime();
    bool EnsureShaders(ID3D12Device* device);
    bool EnsureContext(ID3D12Device* device, uint32_t width, uint32_t height);
    void RetireContexts();
    static bool Complete(ID3D12Fence* fence, uint64_t value);
    ffx::Context context{};
    std::vector<Retired> retired;
    ID3D12Device* contextDevice = nullptr;
    uint32_t contextWidth = 0, contextHeight = 0;
    HMODULE loaderModule = nullptr, denoiserModule = nullptr;
    ffxFunctions runtimeApi{};
    bool sdkFailed = false;
    winrt::com_ptr<ID3D12Fence> lastContextFence;
    uint64_t lastContextValue = 0;
    uint32_t lastFrame = 0, previousFrame = 0, previousWidth = 0, previousHeight = 0;
    uint64_t lastSceneRevision = 0, previousSceneRevision = 0;
    bool lastFrameValid = false, previousValid = false;
    winrt::com_ptr<ID3D12Resource> previousDepth;
    winrt::com_ptr<ID3D12RootSignature> guideRoot, packRoot;
    winrt::com_ptr<ID3D12PipelineState> guidePso, packPso;
    ID3D12Device* shaderDevice = nullptr;
    uint64_t dispatches = 0, bypasses = 0;
    std::string status = "Shadow RR disabled";
};

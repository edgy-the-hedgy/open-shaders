#pragma once
#include "DX12NativeScene.h"
#include "Features/Upscaling/DX12SwapChain.h"
#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <vector>
#include <winrt/base.h>

/** @brief Independent reconstruction and final-color application of raw Native Scene shadows. */
struct DX12NativeComposite final : Feature
{
    ~DX12NativeComposite();
    struct FinalFrame
    {
        std::shared_ptr<WrappedResource> color;
        winrt::com_ptr<ID3D12Fence> producerFence;
        uint64_t producerValue = 0;
        uint32_t frame = 0, width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };
    std::shared_ptr<const FinalFrame> CaptureAndApply(ID3D11Texture2D* raster,
        uint32_t frame, uint32_t width, uint32_t height);
    std::shared_ptr<const FinalFrame> GetFinishedColor(uint32_t frame) const
    { return finishedColor && finishedColor->frame == frame ? finishedColor : nullptr; }
    bool RetainFinishedColor(const std::shared_ptr<const FinalFrame>& frame,
        ID3D12Fence* consumer, uint64_t value);
    std::string GetName() override { return "DX12 Native Composite"; }
    std::string GetShortName() override { return "DX12NativeComposite"; }
    std::string_view GetCategory() const override { return FeatureCategories::kDX12; }
    bool IsCore() const override { return true; }
    bool SupportsVR() override { return false; }
    bool IsAlwaysEnabled() const override { return true; }
    bool IsInMenu() const override { return false; }
    bool IsInFeatureList() const override { return false; }
    void DrawSettings() override;
    void LoadSettings(json& input) override;
    void SaveSettings(json& output) override;
    void RestoreDefaultSettings() override;
    bool WantsPreShadowMapsHook() const override { return true; }
    void PreShadowMaps() override;
    void OnRuntimeDisabled() override;
    void OnSceneTransitionReset(bool opening) override;
    bool IsActive() const;
    bool ShouldDisableMatchedRasterShadows() const;
    bool MatchesNativeShadowMode(const RE::BSShadowLight* light) const;
    bool ForceStaticPreparedFlicker() const
    { return includeFlickeringLights.load(std::memory_order_acquire); }
private:
    struct CompositeSlot
    {
        std::unique_ptr<WrappedResource> input;
        std::shared_ptr<WrappedResource> output;
        std::shared_ptr<FinalFrame> frame;
        std::shared_ptr<const DX12::RawShadowFrame> raw;
        winrt::com_ptr<ID3D12Resource> filtered, camera;
        winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
        winrt::com_ptr<ID3D12CommandAllocator> allocator;
        winrt::com_ptr<ID3D12GraphicsCommandList4> commands;
        SharedFence handoff;
        D3D11_TEXTURE2D_DESC desc{};
        uint32_t maskBatches = 0, renderWidth = 0, renderHeight = 0;
        std::vector<std::pair<winrt::com_ptr<ID3D12Fence>, uint64_t>> consumers;
    };
    std::array<std::shared_ptr<CompositeSlot>, 4> compositeSlots;
    std::shared_ptr<FinalFrame> finishedColor;
    winrt::com_ptr<ID3D12RootSignature> applyRoot;
    winrt::com_ptr<ID3D12PipelineState> applyPipeline;
    ID3D12Device* applyDevice = nullptr;
    bool EnsureApplyPipeline(ID3D12Device* device);
    struct PipelineState
    {
        winrt::com_ptr<ID3D12Device> device;
        winrt::com_ptr<ID3D12RootSignature> reconstructionRoot;
        winrt::com_ptr<ID3D12PipelineState> reconstructionPipeline;
    };
    bool EnsurePipelines(ID3D12Device* candidate);
    std::atomic<std::shared_ptr<const PipelineState>> preparedPipelines;
    std::jthread pipelineWorker;
    winrt::com_ptr<ID3D12Device> device;
    winrt::com_ptr<ID3D12RootSignature> reconstructionRoot;
    winrt::com_ptr<ID3D12PipelineState> reconstructionPipeline;
    std::atomic<bool> pipelineFailed = false;
    std::atomic<std::shared_ptr<const std::string>> pipelineFailureReason;
    std::atomic<bool> spatialReconstruction{true};
    std::atomic<float> shadowIntensity{1.0f}, shadowBlend{1.0f};
    std::atomic<bool> diagnosticDarkenEnabled{false};
    std::atomic<int> diagnosticSelectedLight{-1};
    std::atomic<bool> disableMatchedRasterStatic{false};
    std::atomic<bool> includeFlickeringLights{false};
    bool previousRasterSuppressionArmed = false;
};

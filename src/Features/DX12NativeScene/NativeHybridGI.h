#pragma once
#include "GraphicsInterop.h"
#include "HybridSamplingPolicy.h"
#include "Features/RTComposer/RTComposer.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct RedstoneGI;
class RedstoneDenoiser;

namespace DX12Native
{
    // Stage 5A: diagnostic GI/AO producer. Only DX12 resources are consumed
    // after the existing one-way DX11 producer-fence handoff. No scene writes.
    class NativeHybridGI
    {
    public:
        static constexpr uint32_t kOutputs = 5;
        enum Output : uint32_t { Specular = 0, Diffuse = 1, Eligibility = 2, DiffuseAO = 3, SpecularAO = 4 };
        struct FrameOutputs
        {
            std::array<winrt::com_ptr<ID3D12Resource>, kOutputs> textures{};
            winrt::com_ptr<ID3D12Resource> constants;
            HybridSamplingLayout sampling{};
            // Logical-grid RR guides: depth, motion, normal/roughness,
            // linear diffuse albedo and specular albedo. GPU-resident only.
            static constexpr uint32_t kGuideCount = 5;
            std::array<winrt::com_ptr<ID3D12Resource>, kGuideCount> rrGuides{};
            std::array<uint32_t, 2> rrGuideSize{};
            bool rrGuidesValid = false;
            winrt::com_ptr<ID3D12Resource> rrValidatedMotion;
            bool rrHistoryValid = false;
            // 0 diffuse, 1 specular, 2 AO, 3 specular AO.
            static constexpr uint32_t kSignals = 4;
            std::array<winrt::com_ptr<ID3D12Resource>, kSignals> rrOutputs{};
            std::array<winrt::com_ptr<ID3D12Resource>, kSignals> nativeFiltered{};
            std::array<winrt::com_ptr<ID3D12Resource>, kSignals> nativeBlurred{};
            std::array<ID3D12Resource*, kSignals> nativeFinal{};
            // Temporal native resolve is a separate logical-resolution source;
            // AMD RR output retains its own texture and resource ownership.
            std::array<ID3D12Resource*, kSignals> nativeTemporalFinal{};
            std::array<uint32_t, 2> rrOutputSize{}, postNativeSize{};
            uint32_t advancedTransportTier = 0u;
            bool rrValid = false, postValid = false;
            // Native radiance carries alpha=-2 when no valid sample was found.
            uint32_t nativeValidityMask = 0u;
            uint32_t rrFamilySuccessMask = 0u, nativeFamilyFallbackMask = 0u;
            // DX12-produced final color; the sole eventual DX11 export product.
            std::unique_ptr<WrappedResource> composedScene;
            bool valid = false;
            bool composedValid = false;
        };
        static bool ShadersAvailable();
        static bool AdvancedShadersAvailable();
        static uint64_t EstimateBytes(ID3D12Device* device, uint32_t width, uint32_t height,
            const HybridSamplingLayout& sampling,
            const std::array<bool, 3>& postBlurEnabled);
        bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* list,
            FrameOutputs& frame, uint32_t slot, uint32_t frameNumber,
            const std::array<uint32_t, 2>& size,
            const HybridSamplingLayout& sampling,
            ID3D12Resource* linearDepth, ID3D12Resource* normalRough,
            ID3D12Resource* linearRadiance, ID3D12Resource* albedo,
            ID3D12Resource* reflectance, ID3D12Resource* shadowMask,
            const RedstoneGI& settings, float shadowInfluence, float skipThreshold);
        // Convert tier 2..5 DXR sparse diffuse/specular into independently
        // tagged logical-grid RR inputs. AO/SO are deterministic neutral
        // placeholders; RT-exclusive transport does not inherit raster SSGI.
        // This does NOT spatially fill unsampled lobes before the RR dispatch.
        bool RecordAdvancedTransport(ID3D12Device* device,
            ID3D12GraphicsCommandList* list, FrameOutputs& frame,
            uint32_t slot, uint32_t frameNumber,
            std::array<uint32_t, 2> nativeSize, uint32_t transportTier,
            ID3D12Resource* rawDiffuse, ID3D12Resource* rawSpecular,
            ID3D12Resource* receivers, ID3D12Resource* linearDepth);
        const std::string& AdvancedFailure() const { return advancedFailure; }
        uint64_t AdvancedPackDispatches() const { return advancedPackDispatches; }
        // Prepare full logical-grid RR guides from exact representative native
        // pixels, without pre-RR spatial filtering or CPU readback.
        bool RecordRRGuides(ID3D12Device* device, ID3D12GraphicsCommandList* list,
            FrameOutputs& frame, uint32_t slot,
            ID3D12Resource* linearDepth, ID3D12Resource* normalRough,
            ID3D12Resource* motion, ID3D12Resource* diffuseAlbedo,
            ID3D12Resource* specularAlbedo);
        const std::string& GuideFailure() const { return guideFailure; }
        uint64_t GuideDispatches() const { return guideDispatches; }
        bool RecordHistoryValidation(ID3D12Device* device,
            ID3D12GraphicsCommandList* list, FrameOutputs& current,
            uint32_t slot, uint32_t frameNumber, uint64_t sceneGeneration);
        uint64_t HistoryValidationDispatches() const { return historyValidationDispatches; }
        const std::string& HistoryFailure() const { return historyFailure; }
        bool RecordPostRR(ID3D12Device* device, ID3D12GraphicsCommandList* list,
            FrameOutputs& frame, uint32_t slot, uint32_t frameNumber,
            ID3D12Resource* nativeDepth, ID3D12Resource* nativeNormal,
            const RTComposer::CompositionSettings& policy,
            RedstoneDenoiser* denoiser, bool requestRR);
        uint64_t NativeReconstructionDispatches() const { return nativeReconstructionDispatches; }
        uint64_t NativeTemporalDispatches() const { return nativeTemporalDispatches; }
        uint32_t LastRecordedRRFamilyMask() const { return lastRecordedRRFamilyMask; }
        uint32_t LastRecordedNativeFamilyMask() const { return lastRecordedNativeFamilyMask; }
        uint32_t LastRecordedReconstructionFrame() const { return lastRecordedReconstructionFrame; }
        size_t RetainedNativeHistoryResources() const { return retiredNativeHistory.size(); }
        uint64_t RetainedNativeHistoryBytes() const { return retiredNativeHistoryBytes; }
        const std::string& TemporalFailure() const { return temporalFailure; }
        const std::string& ReconstructionProvider() const { return reconstructionProvider; }
        const std::string& ReconstructionFallbackReason() const { return reconstructionFallbackReason; }
        const std::string& PostFailure() const { return postFailure; }
        uint64_t PostDispatches() const { return postDispatches; }
        uint64_t BlurDispatches() const { return blurDispatches; }
        // Records only when GI and AO were produced for this frame. No CPU readback.
        bool RecordComposition(ID3D12Device* device, ID3D11Device5* device11,
            ID3D12GraphicsCommandList* list, FrameOutputs& frame, uint32_t slot,
            const std::array<uint32_t, 2>& size, ID3D12Resource* shadowedSource,
            ID3D12Resource* nativeDirect, ID3D12Resource* nativeShadowMask,
            ID3D12Resource* nativeLinearDepth, ID3D12Resource* diffuseAlbedo, ID3D12Resource* specularAlbedo,
            float directBlend, float giContribution, uint32_t shadowEdgeFilterEnabled,
            float shadowInfluence, uint32_t advancedTier = 0u);
        const std::string& Failure() const { return failure; }
        const std::string& CompositionFailure() const { return compositionFailure; }
        uint64_t CompositionDispatches() const { return compositionDispatches; }
        uint64_t GIDispatches() const { return giDispatches; }
        uint64_t AODispatches() const { return aoDispatches; }
        uint64_t EligibilityDispatches() const { return eligibilityDispatches; }
    private:
        bool Initialize(ID3D12Device* device);
        bool RecordNativeReconstruction(ID3D12Device* device,
            ID3D12GraphicsCommandList* list, FrameOutputs& frame, uint32_t slot,
            uint32_t fallbackFamilyMask);
        bool RecordNativeTemporal(ID3D12Device* device,
            ID3D12GraphicsCommandList* list, FrameOutputs& frame,
            uint32_t slot, uint32_t frameNumber, uint32_t fallbackFamilyMask);
        winrt::com_ptr<ID3D12RootSignature> root;
        std::array<winrt::com_ptr<ID3D12PipelineState>, 2> pipelines;
        winrt::com_ptr<ID3D12PipelineState> eligibilityPipeline;
        winrt::com_ptr<ID3D12RootSignature> advancedPackRoot;
        winrt::com_ptr<ID3D12PipelineState> advancedPackPipeline;
        winrt::com_ptr<ID3D12DescriptorHeap> advancedPackDescriptors;
        winrt::com_ptr<ID3D12RootSignature> guideRoot;
        winrt::com_ptr<ID3D12PipelineState> guidePipeline;
        winrt::com_ptr<ID3D12DescriptorHeap> guideDescriptors;
        winrt::com_ptr<ID3D12RootSignature> historyRoot;
        winrt::com_ptr<ID3D12PipelineState> historyPipeline;
        winrt::com_ptr<ID3D12DescriptorHeap> historyDescriptors;
        // Persistent GPU snapshots are independent of the two recyclable
        // scene frame slots; no CPU readback or cross-API copies.
        std::array<winrt::com_ptr<ID3D12Resource>, 2> historyGuides{};
        std::array<uint32_t, 2> historySize{};
        uint32_t historyLastFrame = UINT32_MAX;
        uint64_t historyGeneration = UINT64_MAX;
        HybridSamplingMode historyMode = HybridSamplingMode::Native;
        uint32_t historyScale = 1u;
        winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
        winrt::com_ptr<ID3D12PipelineState> compositionPipeline;
        winrt::com_ptr<ID3D12RootSignature> compositionRoot; // 10 SRVs, 3 UAVs, CBV b13
        winrt::com_ptr<ID3D12DescriptorHeap> compositionDescriptors;
        winrt::com_ptr<ID3D12RootSignature> nativeReconstructionRoot;
        winrt::com_ptr<ID3D12PipelineState> nativeReconstructionPipeline;
        winrt::com_ptr<ID3D12DescriptorHeap> nativeReconstructionDescriptors;
        winrt::com_ptr<ID3D12RootSignature> temporalRoot;
        winrt::com_ptr<ID3D12PipelineState> temporalPipeline;
        winrt::com_ptr<ID3D12DescriptorHeap> temporalDescriptors;
        // Ping-pong history per native signal, not per AMD SDK context.
        // The first-pass and post-composite NativeHybridGI instances own their
        // histories independently; saved CPU state never reads radiance.
        std::array<std::array<winrt::com_ptr<ID3D12Resource>,
            FrameOutputs::kSignals>, 2> nativeTemporalHistory{};
        std::array<uint32_t, FrameOutputs::kSignals> nativeHistoryLastFrame{
            UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
        std::array<uint64_t, FrameOutputs::kSignals> nativeHistoryDeveloperRevision{};
        std::array<uint32_t, 2> nativeHistorySize{};
        // Previous allocations cannot be destroyed while a queued DX12 frame
        // may still reference them. Retain until the owning instance retires.
        std::vector<winrt::com_ptr<ID3D12Resource>> retiredNativeHistory;
        uint64_t retiredNativeHistoryBytes = 0;
        std::string temporalFailure;
        winrt::com_ptr<ID3D12RootSignature> postRoot;
        std::array<winrt::com_ptr<ID3D12PipelineState>, 2> postPipelines;
        winrt::com_ptr<ID3D12DescriptorHeap> postDescriptors;
        std::string failure, compositionFailure, guideFailure, postFailure, historyFailure, advancedFailure;
        std::string reconstructionProvider, reconstructionFallbackReason;
        uint64_t nativeReconstructionDispatches = 0, nativeTemporalDispatches = 0;
        uint32_t lastRecordedRRFamilyMask = 0u;
        uint32_t lastRecordedNativeFamilyMask = 0u;
        uint32_t lastRecordedReconstructionFrame = UINT32_MAX;
        uint64_t advancedPackDispatches = 0;
        uint64_t giDispatches = 0, aoDispatches = 0, compositionDispatches = 0,
            eligibilityDispatches = 0, guideDispatches = 0,
            postDispatches = 0, blurDispatches = 0,
            historyValidationDispatches = 0;
    };
}

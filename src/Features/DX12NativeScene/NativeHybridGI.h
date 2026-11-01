#pragma once
#include "GraphicsInterop.h"
#include "HybridSamplingPolicy.h"
#include "Features/RTComposer/RTComposer.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>

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
            // 0 diffuse, 1 specular, 2 AO, 3 specular AO.
            static constexpr uint32_t kSignals = 4;
            std::array<winrt::com_ptr<ID3D12Resource>, kSignals> rrOutputs{};
            std::array<winrt::com_ptr<ID3D12Resource>, kSignals> nativeFiltered{};
            std::array<winrt::com_ptr<ID3D12Resource>, kSignals> nativeBlurred{};
            std::array<ID3D12Resource*, kSignals> nativeFinal{};
            std::array<uint32_t, 2> rrOutputSize{}, postNativeSize{};
            bool rrValid = false, postValid = false;
            // DX12-produced final color; the sole eventual DX11 export product.
            std::unique_ptr<WrappedResource> composedScene;
            bool valid = false;
            bool composedValid = false;
        };
        static bool ShadersAvailable();
        static uint64_t EstimateBytes(ID3D12Device* device, uint32_t width, uint32_t height,
            const HybridSamplingLayout& sampling);
        bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* list,
            FrameOutputs& frame, uint32_t slot, uint32_t frameNumber,
            const std::array<uint32_t, 2>& size,
            const HybridSamplingLayout& sampling,
            ID3D12Resource* linearDepth, ID3D12Resource* normalRough,
            ID3D12Resource* linearRadiance, ID3D12Resource* albedo,
            ID3D12Resource* reflectance, ID3D12Resource* shadowMask,
            const RedstoneGI& settings, float shadowInfluence, float skipThreshold);
        // Prepare full logical-grid RR guides from exact representative native
        // pixels, without pre-RR spatial filtering or CPU readback.
        bool RecordRRGuides(ID3D12Device* device, ID3D12GraphicsCommandList* list,
            FrameOutputs& frame, uint32_t slot,
            ID3D12Resource* linearDepth, ID3D12Resource* normalRough,
            ID3D12Resource* motion, ID3D12Resource* diffuseAlbedo,
            ID3D12Resource* specularAlbedo);
        const std::string& GuideFailure() const { return guideFailure; }
        uint64_t GuideDispatches() const { return guideDispatches; }
        bool RecordPostRR(ID3D12Device* device, ID3D12GraphicsCommandList* list,
            FrameOutputs& frame, uint32_t slot, uint32_t frameNumber,
            ID3D12Resource* nativeDepth, ID3D12Resource* nativeNormal,
            const RTComposer::CompositionSettings& policy,
            RedstoneDenoiser& denoiser);
        const std::string& PostFailure() const { return postFailure; }
        uint64_t PostDispatches() const { return postDispatches; }
        uint64_t BlurDispatches() const { return blurDispatches; }
        // Records only when GI and AO were produced for this frame. No CPU readback.
        bool RecordComposition(ID3D12Device* device, ID3D11Device5* device11,
            ID3D12GraphicsCommandList* list, FrameOutputs& frame, uint32_t slot,
            const std::array<uint32_t, 2>& size, ID3D12Resource* shadowedSource,
            ID3D12Resource* nativeDirect, ID3D12Resource* diffuseAlbedo,
            ID3D12Resource* specularAlbedo, float directBlend, float giContribution);
        const std::string& Failure() const { return failure; }
        const std::string& CompositionFailure() const { return compositionFailure; }
        uint64_t CompositionDispatches() const { return compositionDispatches; }
        uint64_t GIDispatches() const { return giDispatches; }
        uint64_t AODispatches() const { return aoDispatches; }
        uint64_t EligibilityDispatches() const { return eligibilityDispatches; }
    private:
        bool Initialize(ID3D12Device* device);
        winrt::com_ptr<ID3D12RootSignature> root;
        std::array<winrt::com_ptr<ID3D12PipelineState>, 2> pipelines;
        winrt::com_ptr<ID3D12PipelineState> eligibilityPipeline;
        winrt::com_ptr<ID3D12RootSignature> guideRoot;
        winrt::com_ptr<ID3D12PipelineState> guidePipeline;
        winrt::com_ptr<ID3D12DescriptorHeap> guideDescriptors;
        winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
        winrt::com_ptr<ID3D12PipelineState> compositionPipeline;
        winrt::com_ptr<ID3D12DescriptorHeap> compositionDescriptors;
        winrt::com_ptr<ID3D12RootSignature> postRoot;
        std::array<winrt::com_ptr<ID3D12PipelineState>, 2> postPipelines;
        winrt::com_ptr<ID3D12DescriptorHeap> postDescriptors;
        std::string failure, compositionFailure, guideFailure, postFailure;
        uint64_t giDispatches = 0, aoDispatches = 0, compositionDispatches = 0,
            eligibilityDispatches = 0, guideDispatches = 0,
            postDispatches = 0, blurDispatches = 0;
    };
}

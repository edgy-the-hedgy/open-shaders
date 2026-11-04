#pragma once
#include "GraphicsInterop.h"
#include "GraphicsBackend.h"
#include "Buffer.h"
#include "SignalGrid.h"
#include "SignalPreparation.h"
#include "Features/RedstoneGI/RuntimeDenoiser.h"
#include "NativeSceneOutput.h"
#include <array>
#include <cstdint>
#include <string>

struct RedstoneGI;

namespace DX12Native
{
    class NativeHybridGI
    {
    public:
        /** @brief Drains this producer's retained frame resources at shutdown. */
        ~NativeHybridGI();
        /** @brief Records raster-sourced GI/AO without requiring a prepared DXR scene or RR. */
        void Dispatch(const GraphicsBackend& backend, const std::array<uint32_t, 2>& size,
            uint32_t frameNumber, const RedstoneGI& settings, const NativeSceneOutput& source);
        /** @brief Releases only fence-complete frame slots. */
        void Retire();
        /** @brief Reports requested, recorded and completed work separately. */
        json GetDiagnostics() const;
        /** @brief Borrows current-frame GI display output for final DX12 composition. */
        std::optional<NativeSceneOutput> GetSceneOutput(uint32_t frameNumber) const;
        /** @brief Retains the frame until final native scene consumption completes. */
        void ReleaseSceneOutput(uint32_t frameNumber, ID3D12Fence* fence = nullptr, uint64_t value = 0);
        /** @brief Releases unused offers before the next frame begins. */
        void DiscardSceneOutputs() { for (auto& frame : frames) frame.awaitingScene = false; }
        /** @brief Retires only native GI signal histories when disabled or after a scene transition. */
        void Reset();
        /** @brief Reloads producer and consumer bytecode while submitted frames retain old pass objects. */
        void ReloadShaders();
        static constexpr uint32_t kOutputs = 5;
        static constexpr uint64_t kCameraSnapshotOffset = 1280;
        enum Output : uint32_t { Specular = 0, Diffuse = 1, Eligibility = 2, DiffuseAO = 3, SpecularAO = 4 };
        struct FrameOutputs
        {
            std::array<winrt::com_ptr<ID3D12Resource>, kOutputs> textures{};
            winrt::com_ptr<ID3D12Resource> constants;
            SignalGrid grid;
            bool valid = false;
            winrt::com_ptr<ID3D12RootSignature> root;
            std::array<winrt::com_ptr<ID3D12PipelineState>, 2> pipelines;
            winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
        };
        /** @brief Checks the two required native estimator binaries. */
        static bool ShadersAvailable();
        /** @brief Estimates committed output allocations for one frame. */
        static uint64_t EstimateBytes(ID3D12Device* device, uint32_t width, uint32_t height);
        /** @brief Records the existing GI and occlusion estimators into retained frame resources. */
        bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* list,
            FrameOutputs& frame, uint32_t slot, uint32_t frameNumber,
            const std::array<uint32_t, 2>& size,
            ID3D12Resource* linearDepth, ID3D12Resource* normalRough,
            ID3D12Resource* linearRadiance, ID3D12Resource* albedo,
            ID3D12Resource* reflectance, const RedstoneGI& settings);
    private:
        struct Frame
        {
            FrameOutputs outputs;
            std::array<std::unique_ptr<WrappedResource>, 6> sources;
            SignalPreparation::Frame rr;
            std::unique_ptr<WrappedResource> composed;
            winrt::com_ptr<ID3D12CommandAllocator> allocator;
            winrt::com_ptr<ID3D12GraphicsCommandList> commands;
            std::unique_ptr<ConstantBuffer> snapshotSettings;
            std::unique_ptr<ConstantBuffer> camera;
            std::array<uint32_t, 2> size{};
            uint64_t fence = 0, retirement = 0;
            bool composedValid = false, motionReady = false;
            bool awaitingScene = false;
            uint32_t number = UINT32_MAX;
            winrt::com_ptr<ID3D12Fence> sceneConsumer;
            uint64_t sceneConsumerValue = 0;
            std::shared_ptr<const void> compositionBindings;
            GraphicsBackend retainedBackend;
            std::shared_ptr<WrappedResource> sceneSource;
        };
        void PrepareInputs(Frame& frame);
        void RecordRR(Frame& frame, uint32_t slot, uint32_t number, const RedstoneGI& settings);
        void RecordComposition(Frame& frame, uint32_t slot, const RedstoneGI& settings);
        uint64_t ResidentBytes() const;
        NativeCompute composition;
        uint64_t scenePublications = 0;
        uint32_t publicationFrame = UINT32_MAX, rrOutputsBoundMask = 0;
        std::string compositionFailure;
        SignalPreparation signalPreparation;
        RedstoneDenoiser nativeDenoiser;
        std::string rrFailure;
        uint64_t historyRevision = 0;
        std::array<Frame, 2> frames;
        GraphicsBackend backend;
        SharedFence producer, completion;
        winrt::com_ptr<ID3D11ComputeShader> prepareInputs;
        uint32_t sourceFrame = UINT32_MAX;
        uint64_t requests = 0, submissions = 0, completions = 0, busyFrames = 0;
        uint32_t lastRequestedFrame = UINT32_MAX;
        bool Initialize(ID3D12Device* device);
        winrt::com_ptr<ID3D12RootSignature> root;
        std::array<winrt::com_ptr<ID3D12PipelineState>, 2> pipelines;
        winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
        std::string failure;
        uint64_t giDispatches = 0, aoDispatches = 0;
    };
}

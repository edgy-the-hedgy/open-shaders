#pragma once
#include "DX12NativeEnvironment.h"
#include "DX12NativeScreenProbe.h"
#include "DX12NativeCharacterCasters.h"
#include <array>
#include <memory>
#include <string>

namespace DX12Native
{
    struct NativeAmbientParameters
    {
        std::array<float4, 3> dalc{};
        float environmentSaturation = 1, skySaturation = 1, dalcAmount = 0, ambientMultiplier = 1;
        float conversionSaturation = 1, utilitySaturation = 1, utilityGammaOffset = 0;
        uint32_t dalcMode = 0;
        uint32_t iblEnabled = 0, dalcValid = 0, pad0 = 0, pad1 = 0;
        float minDiffuseSkyVisibility = 0.1f, minSpecularSkyVisibility = 0.1f, skyMinZ = 0.0f;
        uint32_t traceSkyVisibility = 0;
    };
    static_assert(sizeof(NativeAmbientParameters) == 112);

    // SH is immutable after projection. Every consumer retains the source revision.
    struct EnvironmentIrradiance
    {
        std::shared_ptr<const EnvironmentCube> cube;
        winrt::com_ptr<ID3D12Resource> coefficients;
        uint64_t allocationBytes = 0;
    };

    struct IndirectLightingFrame
    {
        static constexpr auto coverage = MaterialFrameContract::LightingCoverage::EnvironmentOnly;
        static constexpr auto encoding = MaterialFrameContract::SignalEncoding::MaterialModulated;
        static constexpr auto sampling = MaterialFrameContract::SamplingLayout::FullResolution;
        static constexpr auto reconstruction = MaterialFrameContract::ReconstructionState::Raw;
        std::shared_ptr<const NativeScreenProbe::MaterialFrame> material;
        std::shared_ptr<const CharacterCasterFrame> characters;
        std::shared_ptr<const EnvironmentFrame> environment;
        std::shared_ptr<const EnvironmentIrradiance> baseSH, skySH;
        winrt::com_ptr<ID3D12Resource> samples, constants;
        winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
        winrt::com_ptr<ID3D12CommandAllocator> allocator;
        winrt::com_ptr<ID3D12GraphicsCommandList> commands;
        winrt::com_ptr<ID3D12RootSignature> root, projectionRoot;
        winrt::com_ptr<ID3D12PipelineState> pipeline, projectionPipeline;
        winrt::com_ptr<ID3D12Fence> readyFence;
        uint64_t readyValue = 0, allocationBytes = 0;
        uint32_t frame = 0, width = 0, height = 0, rowPitch = 0, available = 0;
        bool skyVisibilityTraced = false;
    };

    // Environment-only raw signals; never advertise complete opaque ownership.
    class NativeIndirectLighting
    {
    public:
        ~NativeIndirectLighting();
        void Poll(uint32_t frame, bool release = false);
        void Reset();
        void Dispatch(const GraphicsBackend&, std::shared_ptr<const NativeScreenProbe::MaterialFrame>,
            std::shared_ptr<const EnvironmentFrame>, uint32_t enabled, float environmentScale, float skyScale, const NativeAmbientParameters& ambient,
            std::shared_ptr<const CharacterCasterFrame> characters = nullptr);
        std::shared_ptr<const IndirectLightingFrame> Get(uint32_t frame) const;
        const std::string& Failure() const { return failure; }
        uint64_t AllocationBytes() const;
        uint64_t Submissions() const { return submissions; }
        uint64_t Projections() const { return projections; }
    private:
        void Initialize(const GraphicsBackend&);
        GraphicsBackend owner;
        VideoMemoryBudget budget;
        winrt::com_ptr<ID3D12RootSignature> root, projectionRoot;
        winrt::com_ptr<ID3D12PipelineState> pipeline, projectionPipeline;
        winrt::com_ptr<ID3D12Fence> completion;
        std::array<std::shared_ptr<IndirectLightingFrame>, 3> slots;
        std::shared_ptr<IndirectLightingFrame> current;
        std::shared_ptr<const EnvironmentIrradiance> baseCache, skyCache;
        uint64_t nextFence = 0, submissions = 0, projections = 0;
        std::string failure;
    };
}

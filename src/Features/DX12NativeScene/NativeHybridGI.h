#pragma once
#include "GraphicsInterop.h"
#include <array>
#include <cstdint>
#include <string>

struct RedstoneGI;

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
            bool valid = false;
        };
        static bool ShadersAvailable();
        static uint64_t EstimateBytes(ID3D12Device* device, uint32_t width, uint32_t height);
        bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* list,
            FrameOutputs& frame, uint32_t slot, uint32_t frameNumber,
            const std::array<uint32_t, 2>& size,
            ID3D12Resource* linearDepth, ID3D12Resource* normalRough,
            ID3D12Resource* linearRadiance, ID3D12Resource* albedo,
            ID3D12Resource* reflectance, ID3D12Resource* shadowMask,
            const RedstoneGI& settings, float shadowInfluence, float skipThreshold);
        const std::string& Failure() const { return failure; }
        uint64_t GIDispatches() const { return giDispatches; }
        uint64_t AODispatches() const { return aoDispatches; }
    private:
        bool Initialize(ID3D12Device* device);
        winrt::com_ptr<ID3D12RootSignature> root;
        std::array<winrt::com_ptr<ID3D12PipelineState>, 2> pipelines;
        winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
        std::string failure;
        uint64_t giDispatches = 0, aoDispatches = 0;
    };
}

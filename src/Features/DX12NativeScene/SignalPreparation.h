#pragma once
#include "NativeCompute.h"
#include "SignalGrid.h"

namespace DX12Native
{
    /** @brief Converts raw signals to SDK encoding without reconstructing or filtering them. */
    class SignalPreparation
    {
    public:
        struct Frame
        {
            SignalGrid grid;
            std::array<winrt::com_ptr<ID3D12Resource>, 5> guides;
            std::array<winrt::com_ptr<ID3D12Resource>, 2> packed;
            winrt::com_ptr<ID3D12Resource> validity, camera;
            std::array<winrt::com_ptr<ID3D12Resource>, 4> outputs;
            uint32_t recordedMask = 0;
            std::shared_ptr<const void> bindings;
            /** @brief Reports actual retained committed surface allocations. */
            uint64_t Bytes(ID3D12Device* device) const;
        };
        /** @brief Budgets optional guides, packed inputs and selected outputs before allocation. */
        static uint64_t EstimateBytes(ID3D12Device* device, const SignalGrid& grid, uint32_t mask);
        /** @brief Prepares stable logical guides and independent packed diffuse/specular validity. */
        bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* commands, Frame& frame,
            uint32_t slot, const SignalGrid& grid, std::span<ID3D12Resource* const> inputs,
            bool dxrMaterials, bool direct, bool packedSource, bool linearLighting, float roughnessFloor,
            uint32_t requestedMask);
        /** @brief Returns shader preparation errors independently from producer status. */
        const std::string& Failure() const { return pipeline.Failure(); }
        /** @brief Reloads bytecode without destroying bindings retained by submitted frames. */
        void Reload() { pipeline.Reset(); }
    private:
        NativeCompute pipeline;
    };
}

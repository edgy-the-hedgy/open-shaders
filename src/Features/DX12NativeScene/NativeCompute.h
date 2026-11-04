#pragma once
#include <d3d12.h>
#include <winrt/base.h>
#include <span>

namespace DX12Native
{
    /** @brief Owns a small native pass with two fence-owned descriptor ranges. */
    class NativeCompute
    {
    public:
        /** @brief Loads packaged bytecode with ten SRVs, eight UAVs, b0 constants and b12 camera data. */
        bool Initialize(ID3D12Device* device, const char* file);
        /** @brief Binds exact resource formats and restores all resources to COMMON after dispatch. */
        void Dispatch(ID3D12Device* device, ID3D12GraphicsCommandList* commands, uint32_t slot,
            std::span<ID3D12Resource* const> reads, std::span<ID3D12Resource* const> writes,
            const std::array<uint32_t, 16>& constants, D3D12_GPU_VIRTUAL_ADDRESS camera,
            const std::array<uint32_t, 2>& size);
        /** @brief Returns the most recent pipeline initialization error. */
        const std::string& Failure() const { return failure; }
    private:
        winrt::com_ptr<ID3D12RootSignature> root;
        winrt::com_ptr<ID3D12PipelineState> pipeline;
        winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
        std::string failure;
    };

    /** @brief Allocates one named GPU-local single-mip UAV texture in COMMON state. */
    winrt::com_ptr<ID3D12Resource> CreateNativeTexture(ID3D12Device* device,
        std::array<uint32_t, 2> size, DXGI_FORMAT format, const wchar_t* name);
    /** @brief Uploads the current non-VR camera ABI to a caller-owned, fence-retained buffer. */
    void SnapshotNativeCamera(ID3D12Device* device, winrt::com_ptr<ID3D12Resource>& output);
}

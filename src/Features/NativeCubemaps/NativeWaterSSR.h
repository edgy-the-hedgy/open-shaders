#pragma once
#include "Features/DX12/DX12NativeEnvironment.h"
#include "Features/DX12/DX12NativeWorldFrame.h"
#include <array>
#include <memory>

namespace DX12Native { struct FullNativeReconstructionFrame; }

namespace NativeCubemaps
{
    // Native execution for the engine's existing water reflection image-space pass.
    // FXC only publishes its raster inputs and copies the computed result.
    class WaterSSR
    {
    public:
        ~WaterSSR();
        void Poll();
        void Draw(const GraphicsBackend&, const DX12Native::NativeWorldFrame&, std::shared_ptr<const DX12Native::EnvironmentFrame>,
            std::shared_ptr<const DX12Native::FullNativeReconstructionFrame>, DX12Native::EnvironmentEncoding,
            bool skyVisible, uint32_t quality, float emitterRadius, ID3D11PixelShader* expected);
        void End();
        void ResetHistory() { latest.reset(); }
        json Diagnostics() const;
    private:
        struct Slot;
        void Initialize(const GraphicsBackend&);
        GraphicsBackend owner;
        DX12Native::VideoMemoryBudget budget;
        SharedFence inputFence, outputFence, consumedFence;
        winrt::com_ptr<ID3D12RootSignature> root;
        winrt::com_ptr<ID3D12PipelineState> pipeline;
        winrt::com_ptr<ID3D11ComputeShader> capture;
        winrt::com_ptr<ID3D11PixelShader> returnShader;
        winrt::com_ptr<ID3D11Buffer> captureCB;
        std::array<std::shared_ptr<Slot>,3> slots;
        std::shared_ptr<Slot> active,latest;
        winrt::com_ptr<ID3D11PixelShader> previousShader;
        winrt::com_ptr<ID3D11ShaderResourceView> previousView;
        uint64_t submissions=0, rejected=0, historySubmissions=0;
        std::string failure;
    };
}

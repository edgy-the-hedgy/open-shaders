#pragma once

#include "Features/DX12/FullNativeFeature.h"
#include "Features/DX12/DX12NativeEnvironment.h"

namespace NativeCubemaps
{
    class CubeProcessing
    {
    public:
        ~CubeProcessing();
        bool Dispatch(const GraphicsBackend&, std::shared_ptr<const DX12Native::EnvironmentFrame>,
            const std::array<float4,3>&, const float4& cameraPosition);
        void Reset();
        void Poll();
        std::shared_ptr<const DX12Native::EnvironmentFrame> Get() const { return current; }
    private:
        struct Generation;
        void Initialize(const GraphicsBackend&);
        GraphicsBackend owner;
        winrt::com_ptr<ID3D12RootSignature> root;
        winrt::com_ptr<ID3D12PipelineState> historyPipeline, filterPipeline;
        winrt::com_ptr<ID3D12Fence> fence;
        std::array<std::shared_ptr<Generation>,3> slots;
        std::shared_ptr<Generation> latest;
        std::shared_ptr<const DX12Native::EnvironmentFrame> current;
        VideoMemoryBudget budget;
        uint64_t nextFence = 0;
    };

    class Producer final : public DX12::FullNativeFeature
    {
    public:
        std::string GetName() override { return "Native Cubemaps"; }
        std::string GetShortName() override { return "NativeCubemaps"; }
        void OnRuntimeDisabled() override { InvalidateGeneration(); Release(); }
        void ClearShaderCache() override { InvalidateGeneration(); Release(); }
        DX12Native::EnvironmentStamp Prepare(uint32_t scope,uint32_t frame,bool& changed);
        void Capture(const GraphicsBackend&, ID3D11ShaderResourceView* sceneColor, uint32_t frame);
        void InvalidateGeneration();
        void Release();
        std::shared_ptr<const DX12Native::EnvironmentFrame> Get() const { return processing ? processing->Get() : nullptr; }
    private:
        bool CaptureRaw(const GraphicsBackend&, ID3D11ShaderResourceView* sceneColor);
        std::unique_ptr<DX12Native::NativeEnvironment> inputs;
        std::unique_ptr<CubeProcessing> processing;
        winrt::com_ptr<ID3D11Texture2D> raw;
        winrt::com_ptr<ID3D11UnorderedAccessView> rawUAV;
        winrt::com_ptr<ID3D11ComputeShader> inputShader;
        winrt::com_ptr<ID3D11Buffer> inputConstants;
        DX12Native::EnvironmentStamp currentStamp{};
        uint64_t epoch = 1, rawRevision = 0;
        uint32_t lastCaptureFrame = UINT32_MAX;
        float lastHours = 0;
        bool failureLogged = false;
    };
}

#pragma once

#include "Features/DX12/FullNativeFeature.h"
#include "Features/DX12/DX12NativeEnvironment.h"
#include "Features/DX12/VideoMemoryBudget.h"
#include "NativeWaterSSR.h"
#include "NativeCubemapPolicy.h"
#include "NativeForwardEnvironment.h"
#include "Features/IBL/ReflectionRadianceCapture.h"

namespace DX12Native { struct FullNativeReconstructionFrame; }

namespace NativeCubemaps
{
    class CubeProcessing
    {
    public:
        ~CubeProcessing();
        bool Dispatch(const GraphicsBackend&, std::shared_ptr<const DX12Native::EnvironmentFrame>,
            const std::array<float4,3>&, const float4& cameraPosition,
            std::shared_ptr<const DX12Native::FullNativeReconstructionFrame> native);
        void Reset();
        void Poll();
        void Advance(uint32_t frame);
        bool Pending() const { return bool(pending); }
        json GetDiagnostics() const;
        std::shared_ptr<const DX12Native::EnvironmentFrame> Get() const { return current; }
    private:
        struct Generation;
        void Initialize(const GraphicsBackend&);
        void SubmitStep(const std::shared_ptr<Generation>&, uint32_t frame);
        GraphicsBackend owner;
        winrt::com_ptr<ID3D12RootSignature> root;
        winrt::com_ptr<ID3D12PipelineState> historyPipeline, filterPipeline, detectPipeline;
        winrt::com_ptr<ID3D12Fence> fence;
        std::array<std::shared_ptr<Generation>,3> slots;
        std::shared_ptr<Generation> latest, pending;
        uint32_t lastStepFrame = UINT32_MAX;
        uint64_t deferredSteps = 0;
        std::shared_ptr<const DX12Native::EnvironmentFrame> current;
        DX12Native::VideoMemoryBudget budget;
        uint64_t nextFence = 0;
    };

    class Producer final : public DX12::FullNativeFeature
    {
    public:
        std::string GetName() override { return "Native Cubemaps"; }
        std::string GetShortName() override { return "NativeCubemaps"; }
        bool WantsDrawHook() const override { return true; }
        void OnDraw() override;
        bool WantsRenderPassHook() const override { return true; }
        std::function<void()> OnRenderPassBegin(const RE::BSRenderPass*) override { return skyCapture.OnRenderPassBegin(); }
        std::function<void()> OnReflectionsRenderBegin() override;
        void SetupResources() override;
        void OnSceneTransitionReset(bool opening) override { if(opening) InvalidateGeneration(); }
        DX12Native::EnvironmentSource GetSkySource(const DX12Native::EnvironmentStamp&) const;
        ID3D11ShaderResourceView* GetRasterSkyView() const;
        void BeginForward();
        void EndForward();
        void BeginWaterSSR();
        void EndWaterSSR();
        void OnRuntimeDisabled() override { InvalidateGeneration(); Release(); }
        void ClearShaderCache() override { InvalidateGeneration(); Release(); }
        DX12Native::EnvironmentStamp Prepare(uint32_t scope,uint32_t frame,bool& changed);
        void Capture(const GraphicsBackend&, ID3D11ShaderResourceView* sceneColor, uint32_t frame,
            std::shared_ptr<const DX12Native::FullNativeReconstructionFrame> native);
        json GetDiagnostics() override;
        void InvalidateGeneration();
        void Release();
        std::shared_ptr<const DX12Native::EnvironmentFrame> Get() const;
    private:
        bool CaptureRaw(const GraphicsBackend&, ID3D11ShaderResourceView* sceneColor);
        DX12Native::EnvironmentSource GetDefaultSource(const GraphicsBackend&);
        std::unique_ptr<DX12Native::NativeEnvironment> inputs;
        std::unique_ptr<CubeProcessing> processing;
        std::unique_ptr<WaterSSR> waterSSR;
        std::unique_ptr<ForwardEnvironment> forward;
        uint32_t waterScopeDepth = 0;
        bool waterScopeInvalid = false;
        winrt::com_ptr<ID3D11Texture2D> raw, defaultCube;
        bool defaultAttempted=false;
        winrt::com_ptr<ID3D11UnorderedAccessView> rawUAV;
        winrt::com_ptr<ID3D11ComputeShader> inputShader;
        winrt::com_ptr<ID3D11Buffer> inputConstants;
        IBLInput::ReflectionRadianceCapture skyCapture;
        DX12Native::EnvironmentStamp currentStamp{}, skyStamp{};
        Policy::CubeFaceHistory skyFaces;
        uint64_t skyRevision=0;
        uint64_t epoch = 1, rawRevision = 0;
        uint32_t lastCaptureFrame = UINT32_MAX;
        float lastHours = 0;
        bool failureLogged = false;
    };
}

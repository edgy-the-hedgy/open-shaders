#include "NativeHybridGI.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Deferred.h"
#include "Globals.h"
#include "GpuPass.h"
#include "Util.h"

namespace DX12Native
{
    NativeHybridGI::~NativeHybridGI()
    {
        if (completion.fence12 && completion.value)
            completion.CpuWait(completion.value, SharedFence::kFenceTimeoutMs);
    }

    void NativeHybridGI::Retire()
    {
        if (!completion.fence12) return;
        const auto done = completion.fence12->GetCompletedValue();
        if (done == UINT64_MAX) { failure = "Native HybridGI device removed"; return; }
        for (auto& frame : frames) {
            if (!frame.fence || frame.fence > done) continue;
            frame.fence = 0;
            ++completions;
        }
    }

    void NativeHybridGI::PrepareInputs(Frame& frame, const RedstoneGI& settings)
    {
        CS_GPU_PASS("RedstoneGI::NativeSnapshot");
        auto* context = backend.d3d11Context.get();
        auto targets = globals::game::renderer->GetRuntimeData().renderTargets;
        ID3D11ShaderResourceView* reads[]{ Util::GetCurrentSceneDepthSRV(),
            Util::AsReal(targets[NORMALROUGHNESS].SRV),
            Util::AsReal(targets[globals::deferred->forwardRenderTargets[0]].SRV),
            Util::AsReal(targets[ALBEDO].SRV), Util::AsReal(targets[REFLECTANCE].SRV) };
        ID3D11UnorderedAccessView* writes[5]{};
        for (uint32_t i = 0; i < frame.sources.size(); ++i) writes[i] = frame.sources[i]->uav;
        struct Snapshot { uint32_t width, height; float strength; uint32_t padding = 0; };
        frame.snapshotSettings->Update(Snapshot{ frame.size[0], frame.size[1], settings.settings.GIStrength });
        auto* cb = frame.snapshotSettings->CB();
        context->CSSetConstantBuffers(13, 1, &cb);
        context->CSSetShaderResources(0, 5, reads);
        context->CSSetUnorderedAccessViews(0, 5, writes, nullptr);
        context->CSSetShader(prepareInputs.get(), nullptr, 0);
        context->Dispatch((frame.size[0] + 7u) / 8u, (frame.size[1] + 7u) / 8u, 1);
        std::fill(std::begin(reads), std::end(reads), nullptr);
        std::fill(std::begin(writes), std::end(writes), nullptr);
        context->CSSetShaderResources(0, 5, reads);
        context->CSSetUnorderedAccessViews(0, 5, writes, nullptr);
        cb = nullptr;
        context->CSSetConstantBuffers(13, 1, &cb);
    }

    void NativeHybridGI::Dispatch(const GraphicsBackend& graphics, const std::array<uint32_t, 2>& size,
        uint32_t frameNumber, const RedstoneGI& settings)
    {
        CS_GPU_PASS("RedstoneGI::NativeDispatch");
        Retire();
        if (lastRequestedFrame == frameNumber) return;
        lastRequestedFrame = frameNumber;
        ++requests;
        if (!graphics.d3d12Device || !graphics.d3d11Device || !graphics.d3d11Context ||
            !graphics.commandQueue || !size[0] || !size[1]) return;
        const auto available = std::find_if(frames.begin(), frames.end(), [](const Frame& frame) { return !frame.fence; });
        if (available == frames.end()) { ++busyFrames; return; }
        try {
            if (!completion.fence12) {
                backend = graphics;
                producer.Create(backend.d3d12Device.get(), backend.d3d11Device.get(), "RedstoneGI::NativeProducer");
                completion.Create(backend.d3d12Device.get(), backend.d3d11Device.get(), "RedstoneGI::NativeCompletion");
            }
            if (!Initialize(backend.d3d12Device.get())) return;
            if (!prepareInputs) {
                prepareInputs.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
                    L"Data\\Shaders\\RedstoneGI\\nativeInputs.cs.hlsl", {}, "cs_5_0")));
                if (!prepareInputs) throw std::runtime_error("Native GI snapshot shader unavailable");
            }
            auto& frame = *available;
            if (frame.size != size) frame = {};
            if (!frame.allocator) {
                const uint64_t budget = uint64_t(settings.settings.NativeBudgetMB) * 1024 * 1024;
                const uint64_t outputs = EstimateBytes(backend.d3d12Device.get(), size[0], size[1]);
                const uint64_t sourceBytes = uint64_t(size[0]) * size[1] * 36u;
                if (outputs > budget / 2 || sourceBytes > budget / 2 - outputs)
                    throw std::runtime_error("Native GI two-frame allocation exceeds its memory budget");
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = size[0]; desc.Height = size[1];
                desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                constexpr std::array names{ "Depth", "NormalRoughness", "Radiance", "DiffuseAlbedo", "SpecularAlbedo" };
                for (uint32_t i = 0; i < frame.sources.size(); ++i) {
                    desc.Format = i ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32_FLOAT;
                    frame.sources[i] = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(),
                        backend.d3d12Device.get(), std::string("RedstoneGI::Native") + names[i]);
                }
                DX::ThrowIfFailed(backend.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(frame.allocator.put())));
                DX::ThrowIfFailed(backend.d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    frame.allocator.get(), nullptr, IID_PPV_ARGS(frame.commands.put())));
                DX::ThrowIfFailed(frame.commands->Close());
                frame.snapshotSettings = std::make_unique<ConstantBuffer>(ConstantBufferDesc<float4>(), "RedstoneGI::NativeSnapshotSettings");
                frame.size = size;
            }
            PrepareInputs(frame, settings);
            const auto ready = producer.Next();
            DX::ThrowIfFailed(backend.d3d11Context->Signal(producer.fence11.get(), ready));
            backend.d3d11Context->Flush();
            DX::ThrowIfFailed(backend.commandQueue->Wait(producer.fence12.get(), ready));
            DX::ThrowIfFailed(frame.allocator->Reset());
            DX::ThrowIfFailed(frame.commands->Reset(frame.allocator.get(), nullptr));
            const bool recorded = Record(backend.d3d12Device.get(), frame.commands.get(), frame.outputs,
                uint32_t(available - frames.begin()), frameNumber, size,
                frame.sources[0]->resource.get(), frame.sources[1]->resource.get(), frame.sources[2]->resource.get(),
                frame.sources[3]->resource.get(), frame.sources[4]->resource.get(), settings);
            DX::ThrowIfFailed(frame.commands->Close());
            if (!recorded) return;
            ID3D12CommandList* lists[]{ frame.commands.get() };
            backend.commandQueue->ExecuteCommandLists(1, lists);
            frame.fence = completion.Next();
            DX::ThrowIfFailed(backend.commandQueue->Signal(completion.fence12.get(), frame.fence));
            ++submissions;
        } catch (const std::exception& error) { failure = error.what(); }
    }

    json NativeHybridGI::GetDiagnostics() const
    {
        return { { "requests", requests }, { "submissions", submissions }, { "completions", completions },
            { "giDispatchesRecorded", giDispatches }, { "aoDispatchesRecorded", aoDispatches },
            { "busyFrames", busyFrames }, { "failure", failure }, { "requiresPreparedScene", false },
            { "requiresDirect", false }, { "requiresRR", false }, { "sceneConsumed", false } };
    }
}

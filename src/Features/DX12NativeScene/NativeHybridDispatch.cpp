#include "NativeHybridGI.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Features/RTIndirectLighting/RTIndirectLighting.h"
#include "Deferred.h"
#include "Globals.h"
#include "GpuPass.h"
#include "Util.h"
#include "State.h"

namespace DX12Native
{
    void NativeHybridGI::ReloadShaders()
    {
        Reset(); prepareInputs = nullptr; captureSource = nullptr; sourceFrame = UINT32_MAX; root = nullptr; descriptors = nullptr;
        for (auto& value : pipelines) value = nullptr;
        signalPreparation.Reload(); composition.Reset(); failure.clear();
    }
    void NativeHybridGI::Reset()
    {
        ++historyRevision;
        nativeDenoiser.RetireNative();
        for (auto& frame : frames) if (!frame.fence) { frame.rr = {}; frame.sources[5].reset(); }
    }
    NativeHybridGI::~NativeHybridGI()
    {
        if (backend.d3d11Context) backend.d3d11Context->Flush();
        const bool complete = completion.CpuWait(completion.value, SharedFence::kFenceTimeoutMs);
        const bool retired = producer.CpuWait(producer.value, SharedFence::kFenceTimeoutMs);
        if ((!complete || !retired) && backend.d3d12Device && SUCCEEDED(backend.d3d12Device->GetDeviceRemovedReason())) {
            for (auto& frame : frames) if (frame.fence) (void)new Frame(std::move(frame));
            (void)producer.fence11.detach(); (void)producer.fence12.detach();
            (void)completion.fence11.detach(); (void)completion.fence12.detach();
            logger::error("[NativeHybridGI] GPU teardown timed out; in-flight resources retained until process exit");
        }
    }

    void NativeHybridGI::Retire()
    {
        if (!completion.fence12) return;
        const auto done = completion.fence12->GetCompletedValue();
        const auto retired = producer.fence12->GetCompletedValue();
        if (done == UINT64_MAX) { failure = "Native HybridGI device removed"; return; }
        for (auto& frame : frames) {
            if (!frame.fence || frame.fence > done || (frame.retirement && frame.retirement > retired)) continue;
            frame.fence = 0;
            frame.retirement = 0;
            ++completions;
        }
    }

	void NativeHybridGI::CaptureSource(ID3D11ShaderResourceView* diffuse, ID3D11ShaderResourceView* specular,
		const std::array<uint32_t, 2>& size, uint32_t frameNumber)
	{
		CS_GPU_PASS("RedstoneGI::InitiallyLitSnapshot");
		sourceFrame = UINT32_MAX;
		if (!diffuse || !specular || !size[0] || !size[1])
			return;
		try {
			auto* context = globals::d3d::context;
			if (!captureSource) {
				captureSource.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
					L"Data\\Shaders\\RedstoneGI\\nativeSource.cs.hlsl", {}, "cs_5_0")));
				if (!captureSource)
					throw std::runtime_error("Native pre-GI snapshot shader unavailable");
			}
			if (!initialRadiance || sourceSize != size) {
				initialRadiance.reset();
				D3D11_TEXTURE2D_DESC desc{};
				desc.Width = size[0];
				desc.Height = size[1];
				desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
				desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				initialRadiance = std::make_unique<Texture2D>(desc, "RedstoneGI::InitiallyLitRadiance");
				D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = desc.Format;
				srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srv.Texture2D.MipLevels = 1;
				initialRadiance->CreateSRV(srv);
				D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
				uav.Format = desc.Format;
				uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				initialRadiance->CreateUAV(uav);
				sourceSize = size;
			}
			if (!sourceSettings)
				sourceSettings = std::make_unique<ConstantBuffer>(ConstantBufferDesc<float4>(), "RedstoneGI::InitialSourceSettings");
			sourceSettings->Update(std::array<uint32_t, 4>{ size[0], size[1], 0, 0 });
			winrt::com_ptr<ID3D11ComputeShader> previousShader;
			std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> previousReads;
			winrt::com_ptr<ID3D11UnorderedAccessView> previousWrite;
			std::array<winrt::com_ptr<ID3D11Buffer>, 4> previousBuffers;
			constexpr std::array slots{ 5u, 6u, 12u, 13u };
			context->CSGetShader(previousShader.put(), nullptr, nullptr);
			for (uint32_t i = 0; i < 2; ++i) context->CSGetShaderResources(i, 1, previousReads[i].put());
			context->CSGetUnorderedAccessViews(0, 1, previousWrite.put());
			for (uint32_t i = 0; i < 4; ++i) context->CSGetConstantBuffers(slots[i], 1, previousBuffers[i].put());
			globals::state->BindSharedDataCS(context);
			auto* cb = sourceSettings->CB();
			context->CSSetConstantBuffers(13, 1, &cb);
			ID3D11ShaderResourceView* reads[]{ diffuse, specular };
			auto* write = initialRadiance->uav.get();
			context->CSSetShaderResources(0, 2, reads);
			context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
			context->CSSetShader(captureSource.get(), nullptr, 0);
			context->Dispatch((size[0] + 7u) / 8u, (size[1] + 7u) / 8u, 1);
			write = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
			for (uint32_t i = 0; i < 2; ++i) {
				auto* prior = previousReads[i].get();
				context->CSSetShaderResources(i, 1, &prior);
			}
			write = previousWrite.get();
			context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
			for (uint32_t i = 0; i < 4; ++i) {
				auto* prior = previousBuffers[i].get();
				context->CSSetConstantBuffers(slots[i], 1, &prior);
			}
			context->CSSetShader(previousShader.get(), nullptr, 0);
			sourceFrame = frameNumber;
			sourceSelectedProvider = globals::deferred->indirectLighting ? globals::deferred->indirectLighting->GetName() : "none";
		} catch (const std::exception& error) {
			failure = error.what();
		}
	}

	void NativeHybridGI::PrepareInputs(Frame& frame, const RedstoneGI&)
	{
        CS_GPU_PASS("RedstoneGI::NativeSnapshot");
        auto* context = backend.d3d11Context.get();
        std::array<winrt::com_ptr<ID3D11Buffer>, 4> priorBuffers;
        constexpr std::array slots{ 5u, 6u, 12u, 13u };
        for (uint32_t i = 0; i < slots.size(); ++i) context->CSGetConstantBuffers(slots[i], 1, priorBuffers[i].put());
        frame.camera->Update(globals::game::frameBufferCached.nonVR);
        globals::state->BindSharedDataCS(context);
        auto* camera = frame.camera->CB();
        context->CSSetConstantBuffers(12, 1, &camera);
        auto targets = globals::game::renderer->GetRuntimeData().renderTargets;
        ID3D11ShaderResourceView* reads[]{ Util::GetCurrentSceneDepthSRV(),
            Util::AsReal(targets[NORMALROUGHNESS].SRV),
            initialRadiance->srv.get(),
            Util::AsReal(targets[ALBEDO].SRV), Util::AsReal(targets[REFLECTANCE].SRV),
            Util::AsReal(targets[RE::RENDER_TARGET::kMOTION_VECTOR].SRV) };
        ID3D11UnorderedAccessView* writes[6]{};
        for (uint32_t i = 0; i < frame.sources.size(); ++i) writes[i] = frame.sources[i] ? frame.sources[i]->uav : nullptr;
        frame.motionReady = frame.sources[5] && reads[5];
        struct Snapshot { uint32_t width, height; float strength; uint32_t padding = 0; };
        frame.snapshotSettings->Update(Snapshot{ frame.size[0], frame.size[1], 1.0f });
        auto* cb = frame.snapshotSettings->CB();
        context->CSSetConstantBuffers(13, 1, &cb);
        context->CSSetShaderResources(0, 6, reads);
        context->CSSetUnorderedAccessViews(0, 6, writes, nullptr);
        context->CSSetShader(prepareInputs.get(), nullptr, 0);
        context->Dispatch((frame.size[0] + 7u) / 8u, (frame.size[1] + 7u) / 8u, 1);
        std::fill(std::begin(reads), std::end(reads), nullptr);
        std::fill(std::begin(writes), std::end(writes), nullptr);
        context->CSSetShaderResources(0, 6, reads);
        context->CSSetUnorderedAccessViews(0, 6, writes, nullptr);
        cb = nullptr;
        context->CSSetConstantBuffers(13, 1, &cb);
        for (uint32_t i = 0; i < slots.size(); ++i) {
            auto* prior = priorBuffers[i].get();
            context->CSSetConstantBuffers(slots[i], 1, &prior);
        }
    }

    void NativeHybridGI::Dispatch(const GraphicsBackend& graphics, const std::array<uint32_t, 2>& size,
        uint32_t frameNumber, const RedstoneGI& settings, ID3D11UnorderedAccessView* target)
    {
        CS_GPU_PASS("RedstoneGI::NativeDispatch");
        Retire();
        if (lastRequestedFrame == frameNumber) return;
        lastRequestedFrame = frameNumber;
        ++requests;
        if (sourceFrame != frameNumber || sourceSize != size || !initialRadiance) { failure = "Current-frame pre-GI source unavailable"; return; }
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
            const auto requestedResolution = globals::features::rtIndirectLighting.settings.HybridResolution;
            const auto resolution = requestedResolution == 3u ? RayRegeneration::GetSettings().resolutionMode : requestedResolution;
            const auto grid = MakeSignalGrid(size, resolution, settings.settings.NativeCheckerboard, frameNumber);
            if (frame.size != size || frame.outputs.grid.logical != grid.logical || frame.outputs.grid.packed != grid.packed) frame = {};
            frame.outputs.grid = grid;
            frame.retainedBackend = backend;
            if (!frame.allocator) {
                const uint64_t budget = uint64_t(settings.settings.NativeBudgetMB) * 1024 * 1024;
                const uint64_t outputs = EstimateBytes(backend.d3d12Device.get(), grid.packed[0], grid.packed[1]);
                const uint64_t sourceBytes = NativeTextureBytes(backend.d3d12Device.get(), size, DXGI_FORMAT_R32_FLOAT) +
                    5u * NativeTextureBytes(backend.d3d12Device.get(), size, DXGI_FORMAT_R16G16B16A16_FLOAT);
                if (outputs > budget / 2 || sourceBytes > budget / 2 - outputs)
                    throw std::runtime_error("Native GI two-frame allocation exceeds its memory budget");
                if (ResidentBytes() + nativeDenoiser.GetNativeResidentBytes() + outputs + sourceBytes > budget) Reset();
                if (ResidentBytes() + nativeDenoiser.GetNativeResidentBytes() + outputs + sourceBytes > budget)
                    throw std::runtime_error("Native GI raw allocation awaits fence-retired optional resources within its memory budget");
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = size[0]; desc.Height = size[1];
                desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                constexpr std::array names{ "Depth", "NormalRoughness", "Radiance", "DiffuseAlbedo", "SpecularAlbedo", "Motion" };
                for (uint32_t i = 0; i < 5; ++i) {
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
                frame.camera = std::make_unique<ConstantBuffer>(ConstantBufferDesc<globals::FrameBuffer>(), "RedstoneGI::NativeCamera");
                frame.size = size;
            }
            const bool wantsRR = settings.settings.NativeRayRegeneration && RayRegeneration::GetSettings().enabled && settings.GetNativeSignalMask();
            if (!wantsRR) frame.sources[5].reset();
            if (wantsRR && !frame.sources[5]) {
                const auto budget = uint64_t(settings.settings.NativeBudgetMB) * 1024 * 1024;
                const auto motionBytes = NativeTextureBytes(backend.d3d12Device.get(), size, DXGI_FORMAT_R16G16B16A16_FLOAT);
                if (ResidentBytes() + nativeDenoiser.GetNativeResidentBytes() + motionBytes * 2u <= budget) {
                    D3D11_TEXTURE2D_DESC desc{};
                    desc.Width = size[0]; desc.Height = size[1]; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                    try { frame.sources[5] = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "RedstoneGI::NativeMotion"); }
                    catch (const std::exception& error) { rrFailure = error.what(); }
                }
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
            if (recorded) RecordRR(frame, uint32_t(available - frames.begin()), frameNumber, settings);
            if (recorded) RecordComposition(frame, uint32_t(available - frames.begin()), settings);
            DX::ThrowIfFailed(frame.commands->Close());
            if (!recorded) return;
            ID3D12CommandList* lists[]{ frame.commands.get() };
            backend.commandQueue->ExecuteCommandLists(1, lists);
            nativeDenoiser.NotifyNativeSubmitted();
            frame.fence = completion.Next();
            DX::ThrowIfFailed(backend.commandQueue->Signal(completion.fence12.get(), frame.fence));
            ++submissions;
            if (target && frame.composedValid) {
                DX::ThrowIfFailed(backend.d3d11Context->Wait(completion.fence11.get(), frame.fence));
                if (publication.Publish(backend, target, frame.composed->srv, nullptr, size, false, 0.0f, frame.camera->CB())) {
                    ++scenePublications; publicationFrame = frameNumber; rrOutputsBoundMask = frame.rr.recordedMask;
                }
                frame.retirement = producer.Next();
                DX::ThrowIfFailed(backend.d3d11Context->Signal(producer.fence11.get(), frame.retirement));
            }
        } catch (const std::exception& error) {
            nativeDenoiser.CancelNativeRecording(); available->outputs.valid = false;
            available->composedValid = false; failure = error.what();
        }
    }

    json NativeHybridGI::GetDiagnostics() const
    {
        const auto& grid = frames[0].outputs.grid;
        return { { "requests", requests }, { "submissions", submissions }, { "completions", completions },
            { "nativeSize", grid.native }, { "logicalSize", grid.logical }, { "packedSize", grid.packed },
            { "checkerboard", grid.checkerboard }, { "origin", grid.origin },
            { "samplesPerRecordedDispatch", uint64_t(grid.packed[0]) * grid.packed[1] },
            { "giDispatchesRecorded", giDispatches }, { "aoDispatchesRecorded", aoDispatches },
            { "busyFrames", busyFrames }, { "failure", failure }, { "requiresPreparedScene", false },
            { "nativeRR", nativeDenoiser.GetNativeDiagnostics() }, { "rrPreparationFailure", rrFailure },
            { "slot0RRRecordedMask", frames[0].rr.recordedMask }, { "slot1RRRecordedMask", frames[1].rr.recordedMask },
            { "radianceSource", "current-frame pre-indirect raster diffuse plus specular" },
            { "sourceFrame", sourceFrame }, { "sourceIncludesNativeGI", false }, { "sourceIncludesLegacyGIComposite", false },
            { "sourceIncludesNativeDirectOverride", false }, { "selectedLegacyProvider", sourceSelectedProvider },
            { "requiresDirect", false }, { "requiresRR", false }, { "scenePublishSubmissions", scenePublications },
            { "scenePublishFrame", publicationFrame }, { "rrOutputsBoundForComposition", rrOutputsBoundMask },
            { "compositionFailure", compositionFailure }, { "visualConsumptionVerified", false } };
    }
}

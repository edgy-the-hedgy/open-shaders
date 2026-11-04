#include "NativeSceneComposition.h"
#include "Deferred.h"
#include "Features/LinearLighting.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Features/ReverseZ.h"
#include "Globals.h"
#include "GpuPass.h"
#include "NativeSceneOwnership.h"
#include "State.h"
#include "Util.h"
#include <directx/d3dx12.h>

namespace DX12Native
{
	NativeSceneComposition::~NativeSceneComposition()
	{
		if (backend.d3d11Context)
			backend.d3d11Context->Flush();
		const bool complete = completion.CpuWait(completion.value, SharedFence::kFenceTimeoutMs);
		const bool retired = producer.CpuWait(producer.value, SharedFence::kFenceTimeoutMs);
		if (!complete || !retired) {
			for (auto& frame : frames)
				if (frame.fence)
					(void)new Frame(std::move(frame));
			(void)completion.fence11.detach();
			(void)completion.fence12.detach();
			(void)producer.fence11.detach();
			(void)producer.fence12.detach();
		}
	}
	uint64_t NativeSceneComposition::ResidentBytes() const
	{
		if (!backend.d3d12Device)
			return 0;
		uint64_t bytes = 0;
		for (const auto& item : initialSources)
			if (item)
				bytes += NativeAllocationBytes(backend.d3d12Device.get(), item->resource.get());
		for (const auto& item : frames) {
			for (const auto* texture : { item.input.get(), item.output.get(), item.legacy.get(), item.shadowDepth.get() })
				bytes += NativeAllocationBytes(backend.d3d12Device.get(), texture ? texture->resource.get() : nullptr);
			bytes += NativeAllocationBytes(backend.d3d12Device.get(), item.camera.get());
			bytes += NativeAllocationBytes(backend.d3d12Device.get(), item.shadowHistory.get());
		}
		return bytes;
	}
	void NativeSceneComposition::TrimIdleFrames(Frame* keep)
	{
		const auto done = completion.fence12->GetCompletedValue(), retired = producer.fence12->GetCompletedValue();
		for (auto& frame : frames)
			if (&frame != keep && uint32_t(&frame - frames.data()) != shadowHistorySlot && frame.shadowConsumer <= done && (!frame.fence ||
									  (frame.retirement && NativeSceneReusable(false, done, frame.fence, retired, frame.retirement))))
				frame = {};
		for (uint32_t i = 0; i < initialSources.size(); ++i)
			if (i != sourceSlot && initialSources[i] &&
				NativeSceneReusable(initialSources[i].use_count() != 1, retired, sourceReady[i], 0, 0))
				initialSources[i].reset();
	}

	void NativeSceneComposition::Reload()
	{
		composition.Reset();
		shadowResolve.Reset();
		shadowHistorySlot = 2;
		captureSource = nullptr;
		captureShadowDepth = nullptr;
		sourceFrame = UINT32_MAX;
		sourceValid = false;
		legacyShader = nullptr;
	}
	json NativeSceneComposition::GetDiagnostics() const
	{
		return { { "sourceHandoffs", sourceHandoffs }, { "sourceFrame", sourceFrame },
			{ "selectedLegacyProvider", sourceSelectedProvider }, { "shadowApplication", "independent opt-in final-image layer; existing raster shadowing preserved" },
			{ "shadowLayerSubmissions", shadowSubmissions }, { "shadowLayerUnavailable", shadowFailure },
			{ "shadowResolveSubmissions", shadowTemporalSubmissions }, { "shadowResolveFailure", shadowTemporalFailure },
			{ "compositionSubmissions", submissions }, { "returnHandoffs", handoffs }, { "busyFrames", busy },
			{ "failure", failure }, { "source", "current DX11 raster scene, existing shadowing preserved" },
			{ "compositionBackend", "DX12" }, { "visualConsumptionVerified", false } };
	}
	bool NativeSceneComposition::CaptureShadowDepth(Frame& frame, std::array<uint32_t, 2> dimensions, uint64_t budget)
	{
		CS_GPU_PASS("DX12NativeScene::ShadowReceiverDepthSnapshot");
		try {
			auto* depth = Util::GetCurrentSceneDepthSRV();
			if (!depth) throw std::runtime_error("Current composition receiver depth unavailable");
			winrt::com_ptr<ID3D11Resource> source;
			depth->GetResource(source.put());
			auto texture = source.as<ID3D11Texture2D>();
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			D3D11_SHADER_RESOURCE_VIEW_DESC view{};
			depth->GetDesc(&view);
			if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
				desc.Width != dimensions[0] || desc.Height != dimensions[1] || desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.ArraySize != 1)
				throw std::runtime_error(std::format("Shadow receiver depth layout unsupported: {}x{}, samples {}, format {}; scene {}x{}",
					desc.Width, desc.Height, desc.SampleDesc.Count, uint32_t(desc.Format), dimensions[0], dimensions[1]));
			if (!captureShadowDepth)
				captureShadowDepth.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
					L"Data\\Shaders\\RTDirectLighting\\captureShadowDepth.cs.hlsl", {}, "cs_5_0")));
			if (!captureShadowDepth) throw std::runtime_error("Shadow receiver depth capture shader unavailable");
			if (!frame.shadowDepth) {
				const auto bytes = NativeTextureBytes(backend.d3d12Device.get(), dimensions, DXGI_FORMAT_R32_FLOAT);
				if (!NativeSceneBudgetAllows(ResidentBytes(), bytes, budget))
					throw std::runtime_error("Shadow receiver depth exceeds scene composition budget");
				desc.Format = DXGI_FORMAT_R32_FLOAT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				desc.MiscFlags = 0;
				frame.shadowDepth = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "DX12native-shadowmask::CompositionDepth");
			}
			auto* context = backend.d3d11Context.get();
			winrt::com_ptr<ID3D11ComputeShader> priorShader;
			winrt::com_ptr<ID3D11ShaderResourceView> priorRead;
			winrt::com_ptr<ID3D11UnorderedAccessView> priorWrite;
			context->CSGetShader(priorShader.put(), nullptr, nullptr);
			context->CSGetShaderResources(0, 1, priorRead.put());
			context->CSGetUnorderedAccessViews(0, 1, priorWrite.put());
			auto* output = frame.shadowDepth->uav;
			context->CSSetShaderResources(0, 1, &depth);
			context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
			context->CSSetShader(captureShadowDepth.get(), nullptr, 0);
			context->Dispatch((dimensions[0] + 7u) / 8u, (dimensions[1] + 7u) / 8u, 1);
			output = priorWrite.get();
			depth = priorRead.get();
			context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
			context->CSSetShaderResources(0, 1, &depth);
			context->CSSetShader(priorShader.get(), nullptr, 0);
			shadowFailure.clear();
			return true;
		} catch (const std::exception& error) { shadowFailure = error.what(); return false; }
	}
	bool NativeSceneComposition::CaptureLegacy(Frame& frame, std::array<uint32_t, 2> size, float strength)
	{
		CS_GPU_PASS("DX12NativeScene::CaptureLegacyContribution");
		auto& legacy = globals::features::redstoneGI;
		if (!legacy.HasCurrentIndirectLighting())
			return false;
		if (!legacyShader)
			legacyShader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
				L"Data\\Shaders\\RTDirectLighting\\nativeLegacyInput.cs.hlsl", {}, "cs_5_0")));
		if (!legacyShader)
			throw std::runtime_error("Legacy contribution snapshot shader unavailable");
		auto* context = backend.d3d11Context.get();
		if (!frame.legacySettings)
			frame.legacySettings = std::make_unique<ConstantBuffer>(ConstantBufferDesc<float4>(), "DX12NativeScene::LegacyInputSettings");
		if (!frame.legacyCamera)
			frame.legacyCamera = std::make_unique<ConstantBuffer>(ConstantBufferDesc<globals::FrameBuffer>(), "DX12NativeScene::LegacyCamera");
		struct Settings
		{
			std::array<uint32_t, 2> size;
			float strength;
			uint32_t padding = 0;
		};
		frame.legacySettings->Update(Settings{ size, strength });
		frame.legacyCamera->Update(globals::game::frameBufferCached.nonVR);
		winrt::com_ptr<ID3D11ComputeShader> priorShader;
		winrt::com_ptr<ID3D11UnorderedAccessView> priorWrite;
		std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 7> priorReads;
		std::array<winrt::com_ptr<ID3D11Buffer>, 4> priorBuffers;
		constexpr std::array slots{ 0u, 5u, 6u, 12u };
		context->CSGetShader(priorShader.put(), nullptr, nullptr);
		context->CSGetUnorderedAccessViews(0, 1, priorWrite.put());
		for (uint32_t i = 0; i < 7; ++i) context->CSGetShaderResources(i ? 19u + i : 0u, 1, priorReads[i].put());
		for (uint32_t i = 0; i < 4; ++i) context->CSGetConstantBuffers(slots[i], 1, priorBuffers[i].put());
		auto* write = frame.legacy->uav;
		context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
		globals::state->BindSharedDataCS(context);
		auto* cb = frame.legacySettings->CB();
		context->CSSetConstantBuffers(0, 1, &cb);
		cb = frame.legacyCamera->CB();
		context->CSSetConstantBuffers(12, 1, &cb);
		auto* nr = Util::AsReal(globals::game::renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS].SRV);
		context->CSSetShaderResources(0, 1, &nr);
		legacy.BindCompositeResources();
		context->CSSetShader(legacyShader.get(), nullptr, 0);
		context->Dispatch((size[0] + 7u) / 8u, (size[1] + 7u) / 8u, 1);
		legacy.FinishCompositeInputCapture();

		write = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
		for (uint32_t i = 0; i < 7; ++i) {
			auto* read = priorReads[i].get();
			context->CSSetShaderResources(i ? 19u + i : 0u, 1, &read);
		}
		for (uint32_t i = 0; i < 4; ++i) {
			cb = priorBuffers[i].get();
			context->CSSetConstantBuffers(slots[i], 1, &cb);
		}
		write = priorWrite.get();
		context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
		context->CSSetShader(priorShader.get(), nullptr, 0);
		return true;
	}
	std::optional<NativeSceneOutput> NativeSceneComposition::Present(const GraphicsBackend& graphics,
		ID3D11UnorderedAccessView* target, const std::array<uint32_t, 2>& size,
		const std::optional<NativeSceneOutput>& direct, const std::optional<NativeSceneOutput>& hybrid, uint64_t budget,
		const std::optional<NativeShadowLayer>& shadow)
	{
		CS_GPU_PASS("DX12NativeScene::FinalSceneComposition");
		if ((!direct && !hybrid && !shadow) || !target || !graphics.commandQueue || !graphics.d3d12Device)
			return std::nullopt;
		Frame* selected = nullptr;
		try {
			if (completion.fence12 && (backend.d3d12Device != graphics.d3d12Device || backend.d3d11Device != graphics.d3d11Device || backend.commandQueue != graphics.commandQueue))
				throw std::runtime_error("Native scene backend changed; recreate the graphics session");
			if (!completion.fence12) {
				backend = graphics;
				producer.Create(backend.d3d12Device.get(), backend.d3d11Device.get(), "DX12NativeScene::SceneHandoff");
				completion.Create(backend.d3d12Device.get(), backend.d3d11Device.get(), "DX12NativeScene::FinalSceneCompletion");
			}
			if (!shadow || !shadow->temporal) shadowHistorySlot = 2;
			for (uint32_t offset = 1; offset <= frames.size(); ++offset) {
				const auto index = ((shadowHistorySlot < frames.size() ? shadowHistorySlot : uint32_t(frames.size()) - 1u) + offset) % uint32_t(frames.size());
				if (index == shadowHistorySlot) continue;
				auto& frame = frames[index];
				if (frame.shadowConsumer <= completion.fence12->GetCompletedValue() && (!frame.fence || (frame.retirement && NativeSceneReusable(false,
															 completion.fence12->GetCompletedValue(), frame.fence, producer.fence12->GetCompletedValue(), frame.retirement)))) {
					frame.fence = frame.retirement = 0;
					selected = &frame;
					break;
				}
			}
			if (!selected) {
				++busy;
				return std::nullopt;
			}
			auto& frame = *selected;
			if (!composition.Initialize(backend.d3d12Device.get(), "Data/Shaders/RTDirectLighting/nativeSceneCompose.dxil"))
				throw std::runtime_error(composition.Failure());
			winrt::com_ptr<ID3D11Resource> resource;
			target->GetResource(resource.put());
			auto source = resource.as<ID3D11Texture2D>();
			D3D11_TEXTURE2D_DESC desc{};
			source->GetDesc(&desc);
			if (size[0] > desc.Width || size[1] > desc.Height || desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.ArraySize != 1)
				throw std::runtime_error("Unsupported native scene handoff dimensions");
			if (frame.input && (frame.input->resource->GetDesc().Width != desc.Width ||
								   frame.input->resource->GetDesc().Height != desc.Height || frame.input->resource->GetDesc().Format != desc.Format))
				frame = {};
			const bool needsLegacy = direct && direct->legacy && globals::features::redstoneGI.HasCurrentIndirectLighting();
			uint64_t required = 0;
			constexpr uint64_t cameraReserve = 64u * 1024u;
			const std::array<uint32_t, 2> dimensions{ desc.Width, desc.Height };
			if ((!frame.input && (!NativeSceneBudgetAdd(required, NativeTextureBytes(backend.d3d12Device.get(), dimensions, desc.Format), 2u, budget) ||
									 !NativeSceneBudgetAdd(required, cameraReserve, 1u, budget))) ||
				(needsLegacy && !frame.legacy && !NativeSceneBudgetAdd(required, NativeTextureBytes(backend.d3d12Device.get(), dimensions, DXGI_FORMAT_R16G16B16A16_FLOAT), 1u, budget)))
				throw std::runtime_error("Final native scene allocation estimate exceeds its memory budget");
			if (!NativeSceneBudgetAllows(ResidentBytes(), required, budget))
				TrimIdleFrames(&frame);
			if (!NativeSceneBudgetAllows(ResidentBytes(), required, budget))
				throw std::runtime_error("Final native scene surfaces exceed the scene memory budget");
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			desc.MiscFlags = 0;
			if (!frame.input) {
				frame.input = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "DX12NativeScene::RasterSceneInput");
				frame.output = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "DX12NativeScene::FinalScene");
				DX::ThrowIfFailed(backend.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(frame.allocator.put())));
				DX::ThrowIfFailed(backend.d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.allocator.get(), nullptr, IID_PPV_ARGS(frame.commands.put())));
				DX::ThrowIfFailed(frame.commands->Close());
				frame.backend = backend;
			}
			if (needsLegacy && !frame.legacy) {
				desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				frame.legacy = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "DX12NativeScene::LegacyContributionInput");
			}
			auto* context = backend.d3d11Context.get();
			const bool applyShadow = shadow && CaptureShadowDepth(frame, dimensions, budget);
			if (!direct && !hybrid && !applyShadow) return std::nullopt;
			bool legacy = false;
			if (needsLegacy)
				legacy = CaptureLegacy(frame, size, direct->legacyStrength);
			context->CopyResource(frame.input->resource11, source.get());
			const auto ready = producer.Next();
			DX::ThrowIfFailed(context->Signal(producer.fence11.get(), ready));
			context->Flush();
			DX::ThrowIfFailed(backend.commandQueue->Wait(producer.fence12.get(), ready));
			for (const auto* contribution : { direct ? &*direct : nullptr, hybrid ? &*hybrid : nullptr })
				if (contribution)
					DX::ThrowIfFailed(backend.commandQueue->Wait(contribution->ready, contribution->value));
			if (applyShadow) DX::ThrowIfFailed(backend.commandQueue->Wait(shadow->gpu.ready, shadow->gpu.value));
			DX::ThrowIfFailed(frame.allocator->Reset());
			DX::ThrowIfFailed(frame.commands->Reset(frame.allocator.get(), nullptr));
			SnapshotNativeCamera(backend.d3d12Device.get(), frame.camera);
			std::array<D3D12_RESOURCE_BARRIER, 2> barriers{ CD3DX12_RESOURCE_BARRIER::Transition(frame.input->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(frame.output->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST) };
			frame.commands->ResourceBarrier(2, barriers.data());
			frame.commands->CopyResource(frame.output->resource.get(), frame.input->resource.get());
			for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
			frame.commands->ResourceBarrier(2, barriers.data());
			auto* shadowInput = applyShadow ? ResolveShadow(frame, *shadow, size, budget) : nullptr;
			const std::array reads{ frame.input->resource.get(), direct ? direct->first : nullptr, direct ? direct->second : nullptr,
				hybrid ? hybrid->first : nullptr, legacy ? frame.legacy->resource.get() : nullptr,
				shadowInput, applyShadow ? shadow->gpu.second : nullptr,
				applyShadow ? frame.shadowDepth->resource.get() : nullptr };
			const std::array writes{ frame.output->resource.get() };
			const auto& lighting = globals::features::linearLighting;
			const auto bits = [](float value) { return std::bit_cast<uint32_t>(value); };
			const std::array<uint32_t, 20> constants{ size[0], size[1], uint32_t(direct.has_value()), uint32_t(hybrid.has_value()),
				bits(direct ? direct->blend : 0.0f), uint32_t(lighting.IsLinearLightingActive()), uint32_t(lighting.settings.enableACEScg), uint32_t(legacy),
				uint32_t(applyShadow), uint32_t(applyShadow && shadow->preview), bits(applyShadow ? shadow->strength : 0), bits(applyShadow ? shadow->blend : 0),
				bits(applyShadow ? shadow->radius : 0), bits(applyShadow ? shadow->tolerance : 0), uint32_t(globals::features::reverseZ.IsActive()), 0,
				bits(applyShadow ? shadow->origin[0] : 0), bits(applyShadow ? shadow->origin[1] : 0), bits(applyShadow ? shadow->origin[2] : 0), 0 };
			frame.bindings = composition.RetainBindings();
			composition.Dispatch(backend.d3d12Device.get(), frame.commands.get(), uint32_t(selected - frames.data()), reads, writes, constants, frame.camera->GetGPUVirtualAddress(), size);
			DX::ThrowIfFailed(frame.commands->Close());
			frame.fence = completion.Next();
			ID3D12CommandList* lists[]{ frame.commands.get() };
			backend.commandQueue->ExecuteCommandLists(1, lists);
			DX::ThrowIfFailed(backend.commandQueue->Signal(completion.fence12.get(), frame.fence));
			++submissions;
			if (applyShadow) ++shadowSubmissions;
			if (applyShadow && shadowInput == frame.shadowHistory.get()) {
				shadowHistorySlot = uint32_t(selected - frames.data());
				++shadowTemporalSubmissions;
			}
			else shadowHistorySlot = 2;
			if (applyShadow && frame.shadowPreviousSlot < frames.size()) frames[frame.shadowPreviousSlot].shadowConsumer = frame.fence;
			DX::ThrowIfFailed(context->Wait(completion.fence11.get(), frame.fence));
			context->CopyResource(source.get(), frame.output->resource11);
			frame.retirement = producer.Next();
			DX::ThrowIfFailed(context->Signal(producer.fence11.get(), frame.retirement));
			++handoffs;
			failure.clear();
		} catch (const std::exception& error) {
			failure = error.what();
			if (selected && !selected->fence)
				*selected = {};
		}
		if (selected && selected->fence)
			return NativeSceneOutput{ selected->output->resource.get(), nullptr, completion.fence12.get(), selected->fence };
		return std::nullopt;
	}
}

#include "DirectionalTrace.h"

#include "Globals.h"
#include "Features/LinearLighting.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Features/RTComposer/RTComposer.h"
#include "Deferred.h"
#include "Utils/RayRegenerationSettings.h"

#include <fstream>
#include <bit>
#include <BS_thread_pool.hpp>
#include <Windows.h>

namespace DX12Native
{
	bool DirectionalTrace::Initialize(const GraphicsBackend& a_backend, bool a_fsrHybridOnly)
	{
        // Keep the existing GPU backend and fences, without making DXR
        // shaders/geometry prerequisites of raster-sourced FSR HybridGI.
        if (a_fsrHybridOnly) {
            if (completion.fence12 && producer.fence12) return true;
            if (!a_backend.d3d12Device || !a_backend.d3d11Device ||
                !a_backend.d3d11Context || !a_backend.commandQueue) return false;
            backend = a_backend;
            try {
                producer.Create(backend.d3d12Device.get(), backend.d3d11Device.get(),
                    "RTDirectLighting::ReceiverProducer");
                completion.Create(backend.d3d12Device.get(), backend.d3d11Device.get(),
                    "RTDirectLighting::TraceCompletion");
                return true;
            } catch (const std::exception& error) {
                fsrIndependentGIFailure = error.what();
                return false;
            }
        }
		if (!failure.empty()) return false;
		if (pipeline && prepare && display) return true;
		if (!completion.fence12) backend = a_backend;
		try {
			D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
			DX::ThrowIfFailed(backend.d3d12Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
			if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3) throw std::runtime_error("Directional geometry tables require resource binding tier 3");
			D3D12_FEATURE_DATA_SHADER_MODEL model{ D3D_SHADER_MODEL_6_5 };
			DX::ThrowIfFailed(backend.d3d12Device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)));
			if (model.HighestShaderModel < D3D_SHADER_MODEL_6_5) throw std::runtime_error("Directional queries require shader model 6.5");
			if (!completion.fence12) {
				producer.Create(backend.d3d12Device.get(), backend.d3d11Device.get(), "RTDirectLighting::ReceiverProducer");
				completion.Create(backend.d3d12Device.get(), backend.d3d11Device.get(), "RTDirectLighting::TraceCompletion");
			}
			CreatePipeline();
			prepare.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\RTDirectLighting\\prepareReceivers.cs.hlsl", {}, "cs_5_0")));
			display.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\RTDirectLighting\\previewDirectional.cs.hlsl", {}, "cs_5_0")));
			if (!prepare || !display) throw std::runtime_error("Directional receiver/preview shader unavailable");
			return true;
		} catch (const std::exception& error) { failure = error.what(); return false; }
	}

	DirectionalTrace::~DirectionalTrace()
	{
        // A destroyed tracer can still have submitted DX12/DX11 commands.
        // First make a bounded attempt to retire those submissions and
        // release their extensive native/RR allocations instead of
        // intentionally leaking every in-flight frame and its descriptor heaps.
        uint64_t lastCompletion = 0, lastProducer = 0;
        for (const auto& frame : frames) {
            if (!frame.submitted) continue;
            lastCompletion = std::max(lastCompletion, frame.fence);
            lastProducer = std::max(lastProducer, frame.retirement);
        }
        const auto wait = [](ID3D12Fence* fence, uint64_t target) {
            if (target == 0) return true;
            if (!fence) return false;
            if (fence->GetCompletedValue() >= target) return true;
            HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!ready) return false;
            const HRESULT hr = fence->SetEventOnCompletion(target, ready);
            const DWORD status = SUCCEEDED(hr) ?
                WaitForSingleObject(ready, 500) : WAIT_FAILED;
            CloseHandle(ready);
            return status == WAIT_OBJECT_0 && fence->GetCompletedValue() >= target;
        };
        const bool drained =
            wait(completion.fence12.get(), lastCompletion) &&
            wait(producer.fence12.get(), lastProducer);
        if (drained) return; // ordinary RAII releases every resource safely.

        // Never free GPU resources the queue may still reference. Device
        // failure/timeout remains the only exceptional lifetime quarantine.
        const uint64_t done = completion.fence12 ?
            completion.fence12->GetCompletedValue() : 0;
        const uint64_t retired = producer.fence12 ?
            producer.fence12->GetCompletedValue() : 0;
        bool inFlight = false;
        for (auto& frame : frames) {
            if (!frame.submitted || (frame.fence && frame.fence <= done &&
                (!frame.retirement || frame.retirement <= retired))) continue;
            inFlight = true;
            (void)new Frame(std::move(frame));
        }
        if (inFlight) {
            (void)nativeHybridGI.release();
            (void)postHybridGI.release();
            (void)advancedDenoiser.release();
            // A native Direct SDK context or guide snapshot may still be
            // referenced by submitted DX12 work on a failed retirement.
            (void)nativeDirectGuideBuilder.release();
            (void)nativeDirectRRDenoiser.release();
            (void)producer.fence11.detach();
            (void)producer.fence12.detach();
            (void)completion.fence11.detach();
            (void)completion.fence12.detach();
            logger::warn("[NativeDX12] GPU retirement timed out; quarantined in-flight resources for safety");
        }
	}

	void DirectionalTrace::Reset(bool a_releaseBindings)
	{
		++generation;
		previewValid = false;
		rrDebugPreviewValid = false;
		lightSelectionFailure.clear();
        for (auto& frame : frames) {
            frame.shadowTemporalValid = false; // never reuse histories across reset
            // An old producer can still be referenced by an in-flight
            // temporal pass even after its own DX11/DX12 work has retired.
            const auto index = int32_t(&frame - frames.data());
            const bool referenced = std::ranges::any_of(frames, [&](const Frame& other) {
                return &other != &frame && other.submitted &&
                    other.shadowHistorySourceSlot == index;
            });
            if (!frame.submitted && !referenced) frame = {};
        }
		if (a_releaseBindings) cachedBindings.reset();
	}

    bool DirectionalTrace::RetireAdvancedRRContexts()
    {
        // AMD SDK context destruction is guarded by the denoiser's queue
        // fence. On timeout the context remains alive for later retirement.
        return !advancedDenoiser ||
            advancedDenoiser->RetireNativeHybridContexts();
    }

	void DirectionalTrace::ReloadShaders()
	{
		Reset();
		pipeline = nullptr;
		root = nullptr;
		prepare = nullptr;
		display = nullptr;
		composite = nullptr;
        nativeHybridResolve = nullptr;
        nativeHybridPreviewFailure.clear();
        nativeDirectRRComposePipeline = nullptr;
        nativeDirectRRComposeRoot = nullptr;
        nativeDirectRRComposeFailure.clear();
        nativeDirectPackPipeline = nullptr;
        nativeDirectPackRoot = nullptr;
        nativeDirectPackFailure.clear();
        fsrDepthRoot = nullptr;
        fsrDepthPipeline = nullptr;
        fsrIndependentComposeRoot = nullptr;
        fsrIndependentComposePipeline = nullptr;
        prepareDirectGuides = nullptr;
		packRRSignals = nullptr;
		rrDebugShader = nullptr;
		rrDebugFailed = false;
        shadowTemporalPipeline = nullptr;
        shadowTemporalRoot = nullptr;
        shadowTemporalFailure.clear();
		failure.clear();
	}

	void DirectionalTrace::CreatePipeline()
	{
		if (pipeline) return;
		std::ifstream file("Data/Shaders/RTDirectLighting/directional.dxil", std::ios::binary | std::ios::ate);
		if (!file) throw std::runtime_error("Packaged directional.dxil unavailable at Data/Shaders/RTDirectLighting; build RTDirectLightingDXIL and deploy shaders");
		const std::streamoff size = file.tellg();
		constexpr std::streamoff kMaximumShaderBytes = 16 * 1024 * 1024;
		if (size <= 0 || size > kMaximumShaderBytes) throw std::runtime_error("Invalid directional DXIL size");
		std::vector<char> bytecode(static_cast<size_t>(size));
		file.seekg(0);
		if (!file.read(bytecode.data(), std::streamsize(bytecode.size()))) throw std::runtime_error("Cannot read directional DXIL");
		D3D12_DESCRIPTOR_RANGE ranges[6]{};
		ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
		ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1 + kDirectSignalCount + kShadowSignalCount, 0, 0, 0 };
		ranges[2] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaximumInstances, 0, 1, 0 };
		ranges[3] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0, 0 };
		ranges[4] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaximumInstances, 0, 2, 0 };
		ranges[5] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaximumTexturePools, 0, 3, 0 };
		D3D12_ROOT_PARAMETER parameters[12]{};
		parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		parameters[0].Descriptor.ShaderRegister = 0;
		for (uint32_t i = 0; i < 3; ++i) {
			parameters[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[i + 1].DescriptorTable = { 1, &ranges[i] };
		}
		parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		parameters[4].Descriptor.ShaderRegister = 2;
		parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[5].Constants = { 0, 0, 36 };
		parameters[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		parameters[11].Descriptor.ShaderRegister = 6;
		parameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[6].DescriptorTable = { 1, &ranges[3] };
		for (uint32_t i = 0; i < 2; ++i) {
			parameters[7 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[7 + i].DescriptorTable = { 1, &ranges[4 + i] };
			parameters[9 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			parameters[9 + i].Descriptor.ShaderRegister = 4 + i;
		}
		D3D12_STATIC_SAMPLER_DESC samplers[4]{};
		for (uint32_t i = 0; i < 4; ++i) {
			samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			samplers[i].AddressU = (i & 2) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[i].AddressV = (i & 1) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[i].MaxAnisotropy = 1;
			samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
			samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			samplers[i].ShaderRegister = i;
		}
		const D3D12_ROOT_SIGNATURE_DESC desc{ uint32_t(std::size(parameters)), parameters, 4, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE };
		winrt::com_ptr<ID3DBlob> serialized, errors;
		DX::ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
		DX::ThrowIfFailed(backend.d3d12Device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(root.put())));
		root->SetName(L"RTDirectLighting::Directional Root");
		D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
		state.pRootSignature = root.get();
		state.CS = { bytecode.data(), bytecode.size() };
		DX::ThrowIfFailed(backend.d3d12Device->CreateComputePipelineState(&state, IID_PPV_ARGS(pipeline.put())));
		pipeline->SetName(L"RTDirectLighting::Directional Pipeline");
	}

	uint64_t DirectionalTrace::ResidentBytes() const
	{
		uint64_t bytes = previewBytes;
		if (pendingBindings) bytes += pendingBindings->bytes;
		const Bindings* seen[3]{ cachedBindings.get(), nullptr, nullptr };
		uint32_t count = cachedBindings ? 1u : 0u;
		if (cachedBindings) bytes += cachedBindings->bytes;
		for (const auto& frame : frames) {
			bytes += frame.bytes;
			if (frame.bindings && std::find(seen, seen + count, frame.bindings.get()) == seen + count) {
				seen[count++] = frame.bindings.get();
				bytes += frame.bindings->bytes;
			}
		}
		return bytes;
	}

	uint64_t DirectionalTrace::BindingBytes(const GraphicsBackend& a_backend, const TopLevel& a_scene)
	{
		const auto desc = CD3DX12_RESOURCE_DESC::Buffer(a_scene.bottoms.size() * sizeof(uint32_t));
		const auto bytes = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
		const auto material = CD3DX12_RESOURCE_DESC::Buffer(a_scene.instances.size() * sizeof(MaterialRecord));
		const auto surface = CD3DX12_RESOURCE_DESC::Buffer(a_scene.instances.size() * sizeof(SurfaceRecord));
		const auto materialBytes = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &material).SizeInBytes;
		const auto surfaceBytes = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &surface).SizeInBytes;
		return bytes == UINT64_MAX || materialBytes == UINT64_MAX || surfaceBytes == UINT64_MAX ? UINT64_MAX : bytes + materialBytes + surfaceBytes + kBindingOverheadBytes;
	}

	uint64_t DirectionalTrace::MaterialResidentBytes() const
	{
		const MaterialResources* seen[4]{};
		uint32_t count = 0;
		uint64_t bytes = 0;
		const auto add = [&](const std::shared_ptr<const Bindings>& bindings) {
			if (!bindings || !bindings->materials || std::find(seen, seen + count, bindings->materials.get()) != seen + count) return;
			seen[count++] = bindings->materials.get();
			bytes += bindings->materials->bytes;
		};
		add(cachedBindings);
		for (const auto& frame : frames) add(frame.bindings);
		if (pendingBindings && !pendingBindings->busy.load(std::memory_order_acquire)) add(pendingBindings->value);
		return bytes;
	}

	std::shared_ptr<const DirectionalTrace::Bindings> DirectionalTrace::CreateBindings(const GraphicsBackend& a_backend, std::shared_ptr<const TopLevel> a_scene)
	{
		auto result = std::make_shared<Bindings>();
		result->scene = std::move(a_scene);
		result->bytes = BindingBytes(a_backend, *result->scene);
		if (result->scene->bottoms.empty() || result->scene->bottoms.size() > kMaximumInstances ||
			result->scene->bottoms.size() != result->scene->instances.size() || result->bytes == UINT64_MAX)
			throw std::runtime_error("Invalid directional binding size");
		result->materials = MaterialResources::Load(a_backend, *result->scene);
		const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kTextureDescriptorBase + kMaximumTexturePools, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
		DX::ThrowIfFailed(a_backend.d3d12Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(result->heap.put())));
		result->heap->SetName(L"RTDirectLighting::SceneBindings");
		const auto step = a_backend.d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		auto cpu = result->heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += uint64_t(kGeometryDescriptorBase) * step;
		std::vector<uint32_t> offsets;
		offsets.reserve(result->scene->bottoms.size());
		for (const auto& bottom : result->scene->bottoms) {
			const auto& geometry = *bottom->geometry;
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R32_TYPELESS;
			srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Buffer.NumElements = uint32_t(geometry.buffer->GetDesc().Width / sizeof(uint32_t));
			srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			a_backend.d3d12Device->CreateShaderResourceView(geometry.buffer.get(), &srv, cpu);
			cpu.ptr += step;
			offsets.push_back(uint32_t(geometry.validationOffset));
		}
		cpu = result->heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += uint64_t(kAttributeDescriptorBase) * step;
		for (const auto& instance : result->scene->instances) {
			const auto attributes = instance.material.attributes;
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R32_TYPELESS;
			srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Buffer.NumElements = attributes ? uint32_t(attributes->buffer->GetDesc().Width / 4) : 1;
			srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			a_backend.d3d12Device->CreateShaderResourceView(attributes ? attributes->buffer.get() : nullptr, &srv, cpu);
			cpu.ptr += step;
		}
		cpu = result->heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += uint64_t(kTextureDescriptorBase) * step;
		for (uint32_t i = 0; i < kMaximumTexturePools; ++i) {
			const auto* pool = i < result->materials->pools.size() ? &result->materials->pools[i] : nullptr;
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = pool ? pool->format : DXGI_FORMAT_R8G8B8A8_UNORM;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2DArray.MipLevels = pool ? pool->mips : 1;
			srv.Texture2DArray.ArraySize = pool ? pool->slices : 1;
			a_backend.d3d12Device->CreateShaderResourceView(pool ? pool->resource.get() : nullptr, &srv, cpu);
			cpu.ptr += step;
		}
		const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		const auto upload = CD3DX12_RESOURCE_DESC::Buffer(offsets.size() * sizeof(uint32_t));
		DX::ThrowIfFailed(a_backend.d3d12Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &upload, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(result->offsets.put())));
		result->offsets->SetName(L"RTDirectLighting::ValidationOffsets");
		const D3D12_RANGE noRead{ 0, 0 };
		void* mapped = nullptr;
		DX::ThrowIfFailed(result->offsets->Map(0, &noRead, &mapped));
		std::memcpy(mapped, offsets.data(), offsets.size() * sizeof(uint32_t));
		result->offsets->Unmap(0, nullptr);
		return result;
	}


    void DirectionalTrace::PrepareHybridInputs(Frame& a_frame)
    {
        // Captures the raster scene strictly for diagnostic DX12 consumption.
        // Never overwrites the game's color or changes the active SSGI path.
        a_frame.hybridInputsReady = false;
        if (!prepareHybrid) {
            prepareHybrid.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
                L"Data\\Shaders\\RTDirectLighting\\prepareHybridInputs.cs.hlsl", {}, "cs_5_0")));
            if (!prepareHybrid) {
                hybridInputFailure = "Hybrid capture shader unavailable";
                return;
            }
        }
        auto* context = backend.d3d11Context.get();
        const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
        const std::array<ID3D11ShaderResourceView*, kHybridInputCount> sources{
            Util::AsReal(targets[RE::RENDER_TARGETS::kMAIN].SRV),
            Util::GetCurrentSceneDepthSRV(),
            Util::AsReal(targets[NORMALROUGHNESS].SRV),
            Util::AsReal(targets[ALBEDO].SRV),
            Util::AsReal(targets[REFLECTANCE].SRV),
            Util::AsReal(targets[RE::RENDER_TARGETS::kMOTION_VECTOR].SRV)
        };
        if (std::ranges::any_of(sources, [](auto* value) { return !value; })) {
            hybridInputFailure = "Raster color/depth/material/motion SRV unavailable";
            return;
        }
        std::array<ID3D11UnorderedAccessView*, kHybridInputCount> outputs{};
        for (size_t i = 0; i < kHybridInputCount; ++i)
            outputs[i] = a_frame.hybridInputs[i] ? a_frame.hybridInputs[i]->uav : nullptr;
        if (std::ranges::any_of(outputs, [](auto* value) { return !value; })) {
            hybridInputFailure = "Hybrid capture UAV unavailable";
            return;
        }
        winrt::com_ptr<ID3D11ComputeShader> oldShader;
        std::array<winrt::com_ptr<ID3D11ShaderResourceView>, kHybridInputCount> oldSRVs;
        std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, kHybridInputCount> oldUAVs;
        context->CSGetShader(oldShader.put(), nullptr, nullptr);
        for (uint32_t i = 0; i < kHybridInputCount; ++i) {
            context->CSGetShaderResources(i, 1, oldSRVs[i].put());
            context->CSGetUnorderedAccessViews(i, 1, oldUAVs[i].put());
        }
        // The deferred renderer may still have kMAIN bound as u0. Explicitly
        // release its UAV binding before sampling the very same texture as t0.
        ID3D11UnorderedAccessView* clearUAVs[kHybridInputCount]{};
        context->CSSetUnorderedAccessViews(0, kHybridInputCount, clearUAVs, nullptr);
        context->CSSetShaderResources(0, kHybridInputCount, sources.data());
        context->CSSetUnorderedAccessViews(0, kHybridInputCount, outputs.data(), nullptr);
        context->CSSetShader(prepareHybrid.get(), nullptr, 0);
        context->Dispatch((a_frame.size[0] + 7u) / 8u, (a_frame.size[1] + 7u) / 8u, 1);
        ID3D11ShaderResourceView* clearSRVs[kHybridInputCount]{};
        context->CSSetShaderResources(0, kHybridInputCount, clearSRVs);
        context->CSSetUnorderedAccessViews(0, kHybridInputCount, clearUAVs, nullptr);
        for (uint32_t i = 0; i < kHybridInputCount; ++i) {
            auto* uav = oldUAVs[i].get();
            auto* srv = oldSRVs[i].get();
            context->CSSetUnorderedAccessViews(i, 1, &uav, nullptr);
            context->CSSetShaderResources(i, 1, &srv);
        }
        context->CSSetShader(oldShader.get(), nullptr, 0);
        a_frame.hybridInputsReady = true;
        hybridInputFailure.clear();
        hybridInputLastFrame = a_frame.number;
        ++hybridInputDispatches;
    }

	void DirectionalTrace::PrepareReceivers(Frame& a_frame, const std::array<uint32_t, 2>& a_nativeSize, const LightLimitFixData::LightView& a_lights, ID3D11ShaderResourceView* a_roomMetadata)
	{
		CS_GPU_PASS("RTDirectLighting::PrepareReceivers");
		auto create = [&](const void* data, uint32_t bytes, winrt::com_ptr<ID3D11Buffer>& output, const char* name) {
			if (output) { backend.d3d11Context->UpdateSubresource(output.get(), 0, nullptr, data, 0, 0); return; }
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = bytes;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			const D3D11_SUBRESOURCE_DATA initial{ data, 0, 0 };
			DX::ThrowIfFailed(backend.d3d11Device->CreateBuffer(&desc, &initial, output.put()));
			Util::SetResourceName(output.get(), name);
		};
		struct Grid
		{
			uint32_t width, height, nativeWidth, nativeHeight;
			uint32_t lightReference, lightBase, roomGeneration, roomMetadata;
			std::array<float, 3> eyeOrigin;
			uint32_t lightCount;
			uint32_t requestedLightCount, hasShadowMotion, padding[2]{};
			std::array<std::array<uint32_t, 4>, kMaximumDirectLights> identities{};
			std::array<std::array<uint32_t, 4>, 128> rooms{};
		};
		Grid grid{ a_frame.size[0], a_frame.size[1], a_nativeSize[0], a_nativeSize[1], a_frame.lightReference, a_frame.lightBase, a_lights.roomGeneration, a_roomMetadata ? 1u : 0u, a_lights.eyeOrigin, a_lights.count, uint32_t(a_frame.lightIdentities.size()) };
        auto* engineMotion = Util::AsReal(
            globals::game::renderer->GetRuntimeData().renderTargets[
                RE::RENDER_TARGETS::kMOTION_VECTOR].SRV);
        a_frame.shadowMotionCaptured = a_frame.shadowTemporalEnabled &&
            a_frame.shadowMotion && engineMotion;
        grid.hasShadowMotion = a_frame.shadowMotionCaptured ? 1u : 0u;
		for (size_t index = 0; index < a_frame.lightIdentities.size(); ++index)
			grid.identities[index] = { a_frame.lightIdentities[index][0], a_frame.lightIdentities[index][1], 0, 0 };
		for (uint32_t room = 0; room < grid.rooms.size(); ++room) grid.rooms[room] = { a_frame.roomReferences[room], a_lights.roomReferences[room], 0, 0 };
		static_assert(sizeof(Grid) == 64 + kMaximumDirectLights * 16 + 128 * 16);
		create(&grid, sizeof(grid), a_frame.grid, "RTDirectLighting::ReceiverGrid CB");
		create(&globals::game::frameBufferCached.nonVR, sizeof(globals::FrameBuffer), a_frame.camera, "RTDirectLighting::ReceiverCamera CB");
		winrt::com_ptr<ID3D11ComputeShader> shader;
		winrt::com_ptr<ID3D11ShaderResourceView> input, lightInput, roomInput, oldMotionInput;
		winrt::com_ptr<ID3D11UnorderedAccessView> output, lightOutput, oldMotionOutput;
		winrt::com_ptr<ID3D11Buffer> oldGrid, oldCamera, oldShared, oldFeatures;
		auto* context = backend.d3d11Context.get();
		context->CSGetShader(shader.put(), nullptr, nullptr);
		context->CSGetShaderResources(0, 1, input.put());
		context->CSGetShaderResources(1, 1, lightInput.put());
		context->CSGetShaderResources(2, 1, roomInput.put());
        context->CSGetShaderResources(3, 1, oldMotionInput.put());
		context->CSGetUnorderedAccessViews(0, 1, output.put());
		context->CSGetUnorderedAccessViews(1, 1, lightOutput.put());
        context->CSGetUnorderedAccessViews(2, 1, oldMotionOutput.put());
		context->CSGetConstantBuffers(0, 1, oldGrid.put());
		context->CSGetConstantBuffers(12, 1, oldCamera.put());
		context->CSGetConstantBuffers(5, 1, oldShared.put());
		context->CSGetConstantBuffers(6, 1, oldFeatures.put());
		auto* depth = Util::GetCurrentSceneDepthSRV();
		if (!depth) throw std::runtime_error("Native receiver depth unavailable");
		auto* gridBuffer = a_frame.grid.get();
		auto* cameraBuffer = a_frame.camera.get();
		ID3D11ShaderResourceView* inputs[]{ depth, nullptr, a_roomMetadata,
            a_frame.shadowMotionCaptured ? engineMotion : nullptr };
		ID3D11UnorderedAccessView* outputs[]{ a_frame.receivers->uav, a_frame.light->uav,
            a_frame.shadowMotionCaptured ? a_frame.shadowMotion->uav : nullptr };
		context->CSSetShaderResources(0, 4, inputs);
		context->CSSetUnorderedAccessViews(0, 3, outputs, nullptr);
		context->CSSetConstantBuffers(0, 1, &gridBuffer);
		context->CSSetConstantBuffers(12, 1, &cameraBuffer);
		globals::state->BindSharedDataCS(context);
		context->CSSetShader(prepare.get(), nullptr, 0);
		a_frame.submitted = true;
		context->Dispatch((std::max({ a_frame.size[0], uint32_t(a_frame.lightIdentities.size()), 128u }) + 7) / 8, (a_frame.size[1] + 7) / 8, 1);
		ID3D11UnorderedAccessView* priorOutputs[]{ output.get(), lightOutput.get(), oldMotionOutput.get() };
		ID3D11ShaderResourceView* priorInputs[]{ input.get(), lightInput.get(), roomInput.get(), oldMotionInput.get() };
		gridBuffer = oldGrid.get(); cameraBuffer = oldCamera.get();
		context->CSSetUnorderedAccessViews(0, 3, priorOutputs, nullptr);
		context->CSSetShaderResources(0, 4, priorInputs);
		context->CSSetConstantBuffers(0, 1, &gridBuffer);
		context->CSSetConstantBuffers(12, 1, &cameraBuffer);
		ID3D11Buffer* priorShared[]{ oldShared.get(), oldFeatures.get() };
		context->CSSetConstantBuffers(5, 2, priorShared);
		context->CSSetShader(shader.get(), nullptr, 0);
		if (a_frame.hybridInputs[0]) PrepareHybridInputs(a_frame);
		// The one existing producer fence covers receiver and Hybrid input writes.
		const auto produced = producer.Next();
		DX::ThrowIfFailed(context->Signal(producer.fence11.get(), produced));
		context->Flush();
		DX::ThrowIfFailed(backend.commandQueue->Wait(producer.fence12.get(), produced));
	}



    void DirectionalTrace::RecordShadowTemporal(Frame& frame)
    {
        frame.shadowTemporalValid = false;
        frame.shadowHistorySourceSlot = -1;
        if (!frame.shadowTemporalEnabled || !frame.shadowTemporal ||
            !frame.shadowMask || !frame.shadowMotion || !frame.receivers) return;
        // Only the previous consecutive frame in this compositor generation
        // is eligible. The existing direct queue serializes producer and
        // temporal commands. No CPU wait or readback is introduced.
        const Frame* prior = nullptr;
        if (frame.shadowMotionCaptured)
            for (const auto& candidate : frames)
                if (&candidate != &frame && candidate.shadowTemporalValid &&
                    candidate.shadowTemporal && candidate.receivers &&
                    candidate.generation == frame.generation &&
                    candidate.number + 1u == frame.number &&
                    candidate.size == frame.size &&
                    candidate.lightIdentities == frame.lightIdentities &&
                    candidate.roomReferences == frame.roomReferences)
                    prior = &candidate;
        try {
            if (!shadowTemporalPipeline) {
                std::ifstream file("Data/Shaders/RTDirectLighting/shadowTemporal.dxil",
                    std::ios::binary | std::ios::ate);
                if (!file) throw std::runtime_error("shadowTemporal.dxil not deployed");
                const auto bytes = file.tellg();
                if (bytes <= 0 || bytes > 16 * 1024 * 1024)
                    throw std::runtime_error("Invalid shadow temporal DXIL");
                std::vector<char> code(static_cast<size_t>(bytes));
                file.seekg(0);
                if (!file.read(code.data(), std::streamsize(code.size())))
                    throw std::runtime_error("Cannot load shadow temporal DXIL");
                D3D12_DESCRIPTOR_RANGE ranges[2]{};
                ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0 };
                ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0 };
                D3D12_ROOT_PARAMETER params[3]{};
                params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[0].DescriptorTable = { 1, &ranges[0] };
                params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[1].DescriptorTable = { 1, &ranges[1] };
                params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                params[2].Constants = { 0, 0, 6 };
                const D3D12_ROOT_SIGNATURE_DESC signature{
                    3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE
                };
                winrt::com_ptr<ID3DBlob> serialized, errors;
                DX::ThrowIfFailed(D3D12SerializeRootSignature(&signature,
                    D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
                DX::ThrowIfFailed(backend.d3d12Device->CreateRootSignature(
                    0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                    IID_PPV_ARGS(shadowTemporalRoot.put())));
                D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
                ps.pRootSignature = shadowTemporalRoot.get();
                ps.CS = { code.data(), code.size() };
                DX::ThrowIfFailed(backend.d3d12Device->CreateComputePipelineState(
                    &ps, IID_PPV_ARGS(shadowTemporalPipeline.put())));
            }
            if (!frame.shadowTemporalHeap) {
                const D3D12_DESCRIPTOR_HEAP_DESC desc{
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 7,
                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0
                };
                DX::ThrowIfFailed(backend.d3d12Device->CreateDescriptorHeap(
                    &desc, IID_PPV_ARGS(frame.shadowTemporalHeap.put())));
            }
            auto cpu = frame.shadowTemporalHeap->GetCPUDescriptorHandleForHeapStart();
            const UINT step = backend.d3d12Device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto addSRV = [&](ID3D12Resource* resource, DXGI_FORMAT format) {
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = format;
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                view.Texture2D.MipLevels = 1;
                backend.d3d12Device->CreateShaderResourceView(resource, &view, cpu);
                cpu.ptr += step;
            };
            addSRV(frame.shadowMask.get(), DXGI_FORMAT_R16G16_FLOAT);
            addSRV(prior ? prior->shadowTemporal.get() : nullptr, DXGI_FORMAT_R16G16_FLOAT);
            addSRV(frame.receivers->resource.get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
            addSRV(prior ? prior->receivers->resource.get() : nullptr, DXGI_FORMAT_R32G32B32A32_FLOAT);
            addSRV(frame.shadowMotion->resource.get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
            D3D12_UNORDERED_ACCESS_VIEW_DESC output{};
            output.Format = DXGI_FORMAT_R16G16_FLOAT;
            output.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            backend.d3d12Device->CreateUnorderedAccessView(
                frame.shadowTemporal.get(), nullptr, &output, cpu);
            cpu.ptr += step;
            output.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            backend.d3d12Device->CreateUnorderedAccessView(
                frame.shadowMaskExport ? frame.shadowMaskExport->resource.get() : nullptr,
                nullptr, &output, cpu);

            std::vector<D3D12_RESOURCE_BARRIER> begin, end;
            auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES usage) {
                if (!res) return;
                begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(res,
                    D3D12_RESOURCE_STATE_COMMON, usage));
                end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(res,
                    usage, D3D12_RESOURCE_STATE_COMMON));
            };
            transition(frame.shadowMask.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (prior) transition(prior->shadowTemporal.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transition(frame.receivers->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (prior) transition(prior->receivers->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transition(frame.shadowMotion->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transition(frame.shadowTemporal.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (frame.shadowMaskExport)
                transition(frame.shadowMaskExport->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            frame.shadowTemporalRootRef = shadowTemporalRoot;
            frame.shadowTemporalPipelineRef = shadowTemporalPipeline;
            auto* commands = frame.commands.get();
            commands->ResourceBarrier(uint32_t(begin.size()), begin.data());
            commands->SetPipelineState(shadowTemporalPipeline.get());
            commands->SetComputeRootSignature(shadowTemporalRoot.get());
            ID3D12DescriptorHeap* heaps[]{ frame.shadowTemporalHeap.get() };
            commands->SetDescriptorHeaps(1, heaps);
            auto gpu = frame.shadowTemporalHeap->GetGPUDescriptorHandleForHeapStart();
            commands->SetComputeRootDescriptorTable(0, gpu);
            gpu.ptr += 5u * step;
            commands->SetComputeRootDescriptorTable(1, gpu);
            struct Parameters {
                uint32_t width, height, historyAvailable, exportEnabled;
                float historyWeight, depthTolerance;
            };
            const auto& composer = globals::features::rtComposer;
            const auto& dev = composer.EffectiveDeveloperControls();
            const bool overrideShadow = composer.advancedDeveloperMode &&
                dev.overrideShadowTemporal;
            const Parameters parameters{ frame.size[0], frame.size[1],
                prior != nullptr ? 1u : 0u,
                frame.shadowMaskExport ? 1u : 0u,
                overrideShadow ? dev.shadowTemporalWeight : 0.70f,
                overrideShadow ? dev.shadowTemporalDepthTolerance : 0.018f };
            static_assert(sizeof(Parameters) == sizeof(uint32_t) * 6);
            commands->SetComputeRoot32BitConstants(2, 6, &parameters, 0);
            commands->Dispatch((frame.size[0] + 7u) / 8u,
                (frame.size[1] + 7u) / 8u, 1u);
            commands->ResourceBarrier(uint32_t(end.size()), end.data());
            frame.shadowTemporalValid = true;
            frame.shadowHistorySourceSlot = prior ? int32_t(prior - frames.data()) : -1;
            ++shadowTemporalDispatches;
            if (prior) ++shadowTemporalReprojections;
            shadowTemporalFailure.clear();
        }
        catch (const std::exception& error) {
            // The old mask and DX11 export are still valid. Do not turn a
            // denoiser error into a failure of the production shadow path.
            shadowTemporalFailure = error.what();
        }
    }

    void DirectionalTrace::RecordHybridShadowedSource(Frame& frame)
    {
        frame.hybridShadowedSourceReady = false;
        if (!frame.hybridInputsReady || !frame.hybridInputs[0] ||
            !frame.hybridInputs[1] || !frame.shadowMask ||
            !frame.hybridShadowedSource || !frame.hybridLinearDepth) return;
        try {
            if (!hybridSourcePipeline) {
                std::ifstream file("Data/Shaders/RTDirectLighting/hybridShadowSource.dxil",
                    std::ios::binary | std::ios::ate);
                if (!file) throw std::runtime_error("hybridShadowSource.dxil not deployed");
                const auto bytes = file.tellg();
                if (bytes <= 0 || bytes > 16 * 1024 * 1024)
                    throw std::runtime_error("Invalid hybrid source DXIL size");
                std::vector<char> code(static_cast<size_t>(bytes));
                file.seekg(0);
                if (!file.read(code.data(), std::streamsize(code.size())))
                    throw std::runtime_error("Cannot read hybrid source DXIL");
                D3D12_DESCRIPTOR_RANGE ranges[2]{};
                ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
                ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0 };
                D3D12_ROOT_PARAMETER params[3]{};
                params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[0].DescriptorTable = { 1, &ranges[0] };
                params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[1].DescriptorTable = { 1, &ranges[1] };
                params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                params[2].Constants = { 0, 0, 8 };
                const D3D12_ROOT_SIGNATURE_DESC rs{ 3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
                winrt::com_ptr<ID3DBlob> serialized, errors;
                DX::ThrowIfFailed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                    serialized.put(), errors.put()));
                DX::ThrowIfFailed(backend.d3d12Device->CreateRootSignature(
                    0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                    IID_PPV_ARGS(hybridSourceRoot.put())));
                D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
                ps.pRootSignature = hybridSourceRoot.get();
                ps.CS = { code.data(), code.size() };
                DX::ThrowIfFailed(backend.d3d12Device->CreateComputePipelineState(
                    &ps, IID_PPV_ARGS(hybridSourcePipeline.put())));
                const D3D12_DESCRIPTOR_HEAP_DESC heap{
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5u * uint32_t(frames.size()),
                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0
                };
                DX::ThrowIfFailed(backend.d3d12Device->CreateDescriptorHeap(
                    &heap, IID_PPV_ARGS(hybridSourceHeap.put())));
            }
            const uint32_t index = uint32_t(&frame - frames.data());
            const UINT step = backend.d3d12Device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto cpu = hybridSourceHeap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += uint64_t(5 * index) * step;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            backend.d3d12Device->CreateShaderResourceView(frame.hybridInputs[0]->resource.get(), &srv, cpu);
            cpu.ptr += step;
            srv.Format = DXGI_FORMAT_R16G16_FLOAT;
            backend.d3d12Device->CreateShaderResourceView(EffectiveShadowMask(frame), &srv, cpu);
            cpu.ptr += step;
            srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            backend.d3d12Device->CreateShaderResourceView(frame.hybridInputs[1]->resource.get(), &srv, cpu);
            cpu.ptr += step;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            backend.d3d12Device->CreateUnorderedAccessView(frame.hybridShadowedSource.get(), nullptr, &uav, cpu);
            cpu.ptr += step;
            uav.Format = DXGI_FORMAT_R32_FLOAT;
            backend.d3d12Device->CreateUnorderedAccessView(frame.hybridLinearDepth.get(), nullptr, &uav, cpu);
            auto* commands = frame.commands.get();
            const std::array<D3D12_RESOURCE_BARRIER, 5> begin{
                CD3DX12_RESOURCE_BARRIER::Transition(frame.hybridInputs[0]->resource.get(),
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(EffectiveShadowMask(frame),
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(frame.hybridShadowedSource.get(),
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                CD3DX12_RESOURCE_BARRIER::Transition(frame.hybridInputs[1]->resource.get(),
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(frame.hybridLinearDepth.get(),
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            };
            commands->ResourceBarrier(uint32_t(begin.size()), begin.data());
            commands->SetPipelineState(hybridSourcePipeline.get());
            commands->SetComputeRootSignature(hybridSourceRoot.get());
            ID3D12DescriptorHeap* heaps[]{ hybridSourceHeap.get() };
            commands->SetDescriptorHeaps(1, heaps);
            auto gpu = hybridSourceHeap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += uint64_t(5 * index) * step;
            commands->SetComputeRootDescriptorTable(0, gpu);
            gpu.ptr += 3 * step;
            commands->SetComputeRootDescriptorTable(1, gpu);
            const auto& composer = globals::features::rtComposer;
            const float influence = composer.LegacyHybridPrototypeActive() ?
                composer.composition.fsrHybrid.shadowInfluence :
                (composer.IsFixedPreset() ?
                    RTComposer::GetTierPolicy(composer.AdvancedActive() ?
                        composer.composition.mode :
                        RTComposer::CompositorMode::HybridShadowed).shadowInfluence :
                    composer.composition.shadowInfluence);
            const auto projection = globals::game::frameBufferCached.GetCameraProj();
            const bool reverseDepth = projection._33 * projection._43 < 0.0f;
            const float4 cameraData = Util::GetCameraData();
            uint32_t constants[8]{ frame.size[0], frame.size[1], 0u,
                (reverseDepth ? 1u : 0u) | (composer.IsFixedPreset() ? 2u : 0u) };
            std::memcpy(&constants[2], &influence, sizeof(float));
            std::memcpy(&constants[4], &cameraData, sizeof(cameraData));
            static_assert(sizeof(cameraData) == sizeof(uint32_t) * 4);
            commands->SetComputeRoot32BitConstants(2, 8, constants, 0);
            commands->Dispatch((frame.size[0] + 7u) / 8u, (frame.size[1] + 7u) / 8u, 1);
            const std::array<D3D12_RESOURCE_BARRIER, 5> end{
                CD3DX12_RESOURCE_BARRIER::Transition(frame.hybridInputs[0]->resource.get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                CD3DX12_RESOURCE_BARRIER::Transition(EffectiveShadowMask(frame),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                CD3DX12_RESOURCE_BARRIER::Transition(frame.hybridShadowedSource.get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
                CD3DX12_RESOURCE_BARRIER::Transition(frame.hybridInputs[1]->resource.get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                CD3DX12_RESOURCE_BARRIER::Transition(frame.hybridLinearDepth.get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)
            };
            commands->ResourceBarrier(uint32_t(end.size()), end.data());
            frame.hybridShadowedSourceReady = true;
            hybridSourceLastFrame = frame.number;
            ++hybridSourceDispatches;
            hybridSourceFailure.clear();
        } catch (const std::exception& error) {
            frame.hybridShadowedSourceReady = false;
            hybridSourceFailure = error.what();
        }
    }


    bool DirectionalTrace::RecordFSRRasterDepth(Frame& frame)
    {
        frame.fsrDepthReady = false;
        if (!frame.fsrIndependent || !frame.hybridInputsReady ||
            !frame.hybridInputs[1] || !frame.hybridLinearDepth) return false;
        try {
            auto* device = backend.d3d12Device.get();
            if (!fsrDepthRoot || !fsrDepthPipeline) {
                std::ifstream file("Data/Shaders/RTDirectLighting/fsrRasterDepth.dxil",
                    std::ios::binary | std::ios::ate);
                if (!file) throw std::runtime_error("FSR raster-depth DXIL missing");
                const auto length = file.tellg();
                if (length <= 0 || length > 16 * 1024 * 1024)
                    throw std::runtime_error("Invalid FSR raster-depth DXIL");
                std::vector<char> shader(static_cast<size_t>(length));
                file.seekg(0);
                if (!file.read(shader.data(), std::streamsize(shader.size())))
                    throw std::runtime_error("Cannot read FSR raster-depth DXIL");
                D3D12_DESCRIPTOR_RANGE ranges[2]{};
                ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
                ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
                D3D12_ROOT_PARAMETER params[3]{};
                for (uint32_t i = 0; i < 2; ++i) {
                    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                    params[i].DescriptorTable = { 1, &ranges[i] };
                }
                params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                params[2].Constants = { 0, 0, 8 };
                const D3D12_ROOT_SIGNATURE_DESC signature{
                    3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
                winrt::com_ptr<ID3DBlob> blob, errors;
                DX::ThrowIfFailed(D3D12SerializeRootSignature(&signature,
                    D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put()));
                DX::ThrowIfFailed(device->CreateRootSignature(
                    0, blob->GetBufferPointer(), blob->GetBufferSize(),
                    IID_PPV_ARGS(fsrDepthRoot.put())));
                D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
                desc.pRootSignature = fsrDepthRoot.get();
                desc.CS = { shader.data(), shader.size() };
                DX::ThrowIfFailed(device->CreateComputePipelineState(
                    &desc, IID_PPV_ARGS(fsrDepthPipeline.put())));
            }
            if (!frame.fsrDepthDescriptors) {
                const D3D12_DESCRIPTOR_HEAP_DESC heap{
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2u,
                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
                DX::ThrowIfFailed(device->CreateDescriptorHeap(
                    &heap, IID_PPV_ARGS(frame.fsrDepthDescriptors.put())));
            }
            const UINT step = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto cpu = frame.fsrDepthDescriptors->GetCPUDescriptorHandleForHeapStart();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(frame.hybridInputs[1]->resource.get(), &srv, cpu);
            cpu.ptr += step;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_R32_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(frame.hybridLinearDepth.get(), nullptr, &uav, cpu);
            auto* list = frame.commands.get();
            const std::array<D3D12_RESOURCE_BARRIER, 2> before{
                CD3DX12_RESOURCE_BARRIER::Transition(frame.hybridInputs[1]->resource.get(),
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(frame.hybridLinearDepth.get(),
                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) };
            list->ResourceBarrier(UINT(before.size()), before.data());
            list->SetComputeRootSignature(fsrDepthRoot.get());
            list->SetPipelineState(fsrDepthPipeline.get());
            ID3D12DescriptorHeap* heaps[]{ frame.fsrDepthDescriptors.get() };
            list->SetDescriptorHeaps(1, heaps);
            auto gpu = frame.fsrDepthDescriptors->GetGPUDescriptorHandleForHeapStart();
            list->SetComputeRootDescriptorTable(0, gpu);
            gpu.ptr += step;
            list->SetComputeRootDescriptorTable(1, gpu);
            const auto projection = globals::game::frameBufferCached.GetCameraProj();
            const bool reverseDepth = projection._33 * projection._43 < 0.0f;
            const float4 camera = Util::GetCameraData();
            uint32_t constants[8]{ frame.size[0], frame.size[1],
                reverseDepth ? 1u : 0u, 0u };
            std::memcpy(&constants[4], &camera, sizeof(camera));
            list->SetComputeRoot32BitConstants(2, 8, constants, 0);
            list->Dispatch((frame.size[0] + 7u) / 8u,
                (frame.size[1] + 7u) / 8u, 1u);
            const std::array<D3D12_RESOURCE_BARRIER, 2> after{
                CD3DX12_RESOURCE_BARRIER::Transition(frame.hybridInputs[1]->resource.get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                CD3DX12_RESOURCE_BARRIER::Transition(frame.hybridLinearDepth.get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON) };
            list->ResourceBarrier(UINT(after.size()), after.data());
            frame.fsrDepthRootRef = fsrDepthRoot;
            frame.fsrDepthPipelineRef = fsrDepthPipeline;
            frame.fsrDepthReady = true;
            return true;
        } catch (const std::exception& error) {
            hybridInputFailure = std::string("FSR raster depth: ") + error.what();
            return false;
        }
    }

    bool DirectionalTrace::RecordNativeDirectRRPack(Frame& frame)
    {
        frame.nativeDirectPackValid = false;
        if (!frame.nativeDirectRRRequested) return false;
        if (!frame.commands || !frame.output || frame.size[0] < 2u ||
            (frame.size[0] & 1u) || !frame.size[1]) {
            nativeDirectPackFailure = "Native Direct RR requires even render width";
            return false;
        }
        for (uint32_t i = 0; i < 5u; ++i)
            if (!frame.directSignals[i]) {
                nativeDirectPackFailure = "Direct radiance or material signal missing";
                return false;
            }
        try {
            auto* device = backend.d3d12Device.get();
            auto* list = frame.commands.get();
            if (!nativeDirectPackRoot || !nativeDirectPackPipeline) {
                std::ifstream file("Data/Shaders/RTDirectLighting/nativeDirectRRPack.dxil",
                    std::ios::binary | std::ios::ate);
                if (!file) throw std::runtime_error("Native Direct RR pack DXIL missing");
                const auto byteSize = file.tellg();
                if (byteSize <= 0 || byteSize > 16 * 1024 * 1024)
                    throw std::runtime_error("Invalid native Direct RR pack DXIL");
                std::vector<char> code(static_cast<size_t>(byteSize));
                file.seekg(0);
                if (!file.read(code.data(), std::streamsize(code.size())))
                    throw std::runtime_error("Cannot read native Direct RR pack DXIL");
                D3D12_DESCRIPTOR_RANGE ranges[2]{};
                ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0, 0, 0 };
                ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0 };
                D3D12_ROOT_PARAMETER params[3]{};
                for (uint32_t i = 0; i < 2u; ++i) {
                    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                    params[i].DescriptorTable = { 1, &ranges[i] };
                }
                params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                params[2].Constants = { 0, 0, 8 };
                const D3D12_ROOT_SIGNATURE_DESC signature{
                    3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
                winrt::com_ptr<ID3DBlob> serialized, errors;
                DX::ThrowIfFailed(D3D12SerializeRootSignature(&signature,
                    D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
                DX::ThrowIfFailed(device->CreateRootSignature(0,
                    serialized->GetBufferPointer(), serialized->GetBufferSize(),
                    IID_PPV_ARGS(nativeDirectPackRoot.put())));
                D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
                state.pRootSignature = nativeDirectPackRoot.get();
                state.CS = { code.data(), code.size() };
                DX::ThrowIfFailed(device->CreateComputePipelineState(
                    &state, IID_PPV_ARGS(nativeDirectPackPipeline.put())));
            }
            const uint32_t packedWidth = frame.size[0] / 2u;
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            for (uint32_t i = 0; i < 3u; ++i) {
                if (frame.nativeDirectPacked[i]) continue;
                const DXGI_FORMAT format = i == 2u ?
                    DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16G16B16A16_FLOAT;
                const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                    format, packedWidth, frame.size[1], 1, 1, 1, 0,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                DX::ThrowIfFailed(device->CreateCommittedResource(&heap,
                    D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                    nullptr, IID_PPV_ARGS(frame.nativeDirectPacked[i].put())));
            }
            if (!frame.nativeDirectPackDescriptors) {
                const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 9u,
                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
                DX::ThrowIfFailed(device->CreateDescriptorHeap(
                    &heapDesc, IID_PPV_ARGS(frame.nativeDirectPackDescriptors.put())));
            }
            const std::array<ID3D12Resource*, 6> inputs{
                frame.output->resource.get(),
                frame.directSignals[0]->resource.get(),
                frame.directSignals[1]->resource.get(),
                frame.directSignals[2]->resource.get(),
                frame.directSignals[4]->resource.get(), // Direct Diffuse Albedo
                frame.directSignals[3]->resource.get()  // Direct Specular Albedo
            };
            const UINT step = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto cpu = frame.nativeDirectPackDescriptors->GetCPUDescriptorHandleForHeapStart();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            for (uint32_t i = 0; i < inputs.size(); ++i) {
                srv.Format = i == 0u ? DXGI_FORMAT_R32G32B32A32_FLOAT :
                    DXGI_FORMAT_R16G16B16A16_FLOAT;
                device->CreateShaderResourceView(inputs[i], &srv, cpu);
                cpu.ptr += step;
            }
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            for (uint32_t i = 0; i < 3u; ++i) {
                uav.Format = i == 2u ? DXGI_FORMAT_R32_UINT :
                    DXGI_FORMAT_R16G16B16A16_FLOAT;
                device->CreateUnorderedAccessView(frame.nativeDirectPacked[i].get(),
                    nullptr, &uav, cpu);
                cpu.ptr += step;
            }
            std::array<D3D12_RESOURCE_BARRIER, 9> before{}, after{};
            for (uint32_t i = 0; i < inputs.size(); ++i) {
                before[i] = CD3DX12_RESOURCE_BARRIER::Transition(inputs[i],
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                after[i] = CD3DX12_RESOURCE_BARRIER::Transition(inputs[i],
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            for (uint32_t i = 0; i < 3u; ++i) {
                before[i+6u] = CD3DX12_RESOURCE_BARRIER::Transition(
                    frame.nativeDirectPacked[i].get(), D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                after[i+6u] = CD3DX12_RESOURCE_BARRIER::Transition(
                    frame.nativeDirectPacked[i].get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COMMON);
            }
            list->ResourceBarrier(UINT(before.size()), before.data());
            ID3D12DescriptorHeap* heaps[]{ frame.nativeDirectPackDescriptors.get() };
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(nativeDirectPackRoot.get());
            list->SetPipelineState(nativeDirectPackPipeline.get());
            auto gpu = frame.nativeDirectPackDescriptors->GetGPUDescriptorHandleForHeapStart();
            list->SetComputeRootDescriptorTable(0, gpu);
            gpu.ptr += uint64_t(6u) * step;
            list->SetComputeRootDescriptorTable(1, gpu);
            const uint32_t constants[8]{
                frame.size[0], frame.size[1], packedWidth, frame.size[1],
                frame.number & 1u, 0u, 0u, 0u };
            list->SetComputeRoot32BitConstants(2, 8, constants, 0);
            list->Dispatch((packedWidth + 7u) / 8u,
                (frame.size[1] + 7u) / 8u, 1u);
            list->ResourceBarrier(UINT(after.size()), after.data());
            frame.nativeDirectPackRootRef = nativeDirectPackRoot;
            frame.nativeDirectPackPipelineRef = nativeDirectPackPipeline;
            frame.nativeDirectPackValid = true;
            ++nativeDirectPackDispatches;
            nativeDirectPackFailure.clear();
            return true;
        } catch (const std::exception& error) {
            nativeDirectPackFailure = error.what();
            return false;
        }
    }


    bool DirectionalTrace::RecordNativeDirectRR(Frame& frame)
    {
        frame.nativeDirectRRReady = false;
        frame.nativeDirectRRSuccessMask = 0u;
        if (!frame.nativeDirectRRRequested || !frame.nativeDirectPackValid ||
            !(frame.fsrIndependent ? frame.fsrDepthReady : frame.hybridShadowedSourceReady) ||
            !frame.hybridLinearDepth ||
            !frame.hybridInputsReady || !frame.hybridInputs[5]) {
            nativeDirectRRFailure = "Native Direct RR raw input or guides unavailable";
            ++nativeDirectRRFailedFrames;
            return false;
        }
        try {
            if (!nativeDirectGuideBuilder)
                nativeDirectGuideBuilder = std::make_unique<NativeHybridGI>();
            auto* device = backend.d3d12Device.get();
            auto* list = frame.commands.get();
            auto& guides = frame.nativeDirectGuides;
            const uint32_t slot = uint32_t(&frame - frames.data());
            if (!nativeDirectGuideBuilder->RecordDirectRRGuides(
                    device, list, guides, slot, frame.number, frame.size,
                    frame.hybridLinearDepth.get(),
                    frame.directSignals[2]->resource.get(),
                    frame.hybridInputs[5]->resource.get(),
                    frame.directSignals[4]->resource.get(),
                    frame.directSignals[3]->resource.get())) {
                nativeDirectRRFailure = nativeDirectGuideBuilder->GuideFailure();
                ++nativeDirectRRFailedFrames;
                return false;
            }
            // FSR Independent delegates all temporal history decisions to AMD.
            // No bespoke motion rewriting, depth comparison or normal rejection.
            if (!frame.fsrIndependent) {
                const uint64_t policy = globals::features::rtComposer.developerRevision;
                if (!nativeDirectGuideBuilder->RecordHistoryValidation(
                        device, list, guides, slot, frame.number,
                        frame.generation ^ (policy * 0x9e3779b97f4a7c15ULL))) {
                    nativeDirectRRFailure = nativeDirectGuideBuilder->HistoryFailure();
                    ++nativeDirectRRFailedFrames;
                    return false;
                }
            }
            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            for (uint32_t i = 0; i < 2u; ++i) {
                if (frame.nativeDirectRROutputs[i]) continue;
                const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                    DXGI_FORMAT_R16G16B16A16_FLOAT,
                    frame.size[0], frame.size[1], 1, 1, 1, 0,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                DX::ThrowIfFailed(device->CreateCommittedResource(&heap,
                    D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                    nullptr, IID_PPV_ARGS(frame.nativeDirectRROutputs[i].put())));
            }
            if (!nativeDirectRRDenoiser)
                nativeDirectRRDenoiser = std::make_unique<RedstoneDenoiser>();
            if (!(frame.fsrIndependent ? nativeDirectRRDenoiser->InitializeNative(backend) :
                    nativeDirectRRDenoiser->Initialize(backend))) {
                nativeDirectRRFailure = "AMD native Direct RR initialization unavailable";
                ++nativeDirectRRFailedFrames;
                return false;
            }
            RedstoneDenoiser::NativeHybridResources resources{};
            resources.nativeSize = frame.size;
            resources.logicalSize = frame.size;
            resources.frame = frame.number;
            resources.checkerboard = true;
            resources.checkerboardOrigin = frame.number & 1u;
            resources.resetHistory = !frame.fsrIndependent && !guides.rrHistoryValid;
            resources.skipOcclusion = true;
            resources.directFamilies = true;
            resources.inputs[0] = frame.nativeDirectPacked[0].get();
            resources.inputs[1] = frame.nativeDirectPacked[1].get();
            resources.outputs[0] = frame.nativeDirectRROutputs[0].get();
            resources.outputs[1] = frame.nativeDirectRROutputs[1].get();
            for (uint32_t i = 0; i < resources.guides.size(); ++i)
                resources.guides[i] = guides.rrGuides[i].get();
            if (!frame.fsrIndependent && guides.rrHistoryValid && guides.rrValidatedMotion)
                resources.guides[1] = guides.rrValidatedMotion.get();
            const bool sdkComplete =
                nativeDirectRRDenoiser->DispatchNativeHybridRR(list, resources);
            frame.nativeDirectRRSuccessMask =
                nativeDirectRRDenoiser->NativeHybridSuccessfulFamilyMask() & 3u;
            frame.nativeDirectRRReady =
                sdkComplete && frame.nativeDirectRRSuccessMask == 3u;
            if (!frame.nativeDirectRRReady) {
                nativeDirectRRFailure = nativeDirectRRDenoiser->NativeHybridFailure();
                ++nativeDirectRRFailedFrames;
                return false;
            }
            nativeDirectRRFailure.clear();
            ++nativeDirectRRSuccessfulFrames;
            return true;
        } catch (const std::exception& error) {
            nativeDirectRRFailure = std::string("Direct RR: ") + error.what();
            ++nativeDirectRRFailedFrames;
            return false;
        }
    }


    bool DirectionalTrace::RecordNativeDirectRRComposition(Frame& frame)
    {
        frame.nativeDirectRRComposed = false;
        const auto& composer = globals::features::rtComposer;
        if (!composer.NativeDirectRRPreviewActive() || !frame.nativeDirectRRReady ||
            !frame.hybridShadowedSource || !frame.output ||
            !frame.directSignals[0] || !frame.directSignals[1] ||
            !frame.directSignals[3] || !frame.directSignals[4] ||
            !frame.nativeDirectRROutputs[0] || !frame.nativeDirectRROutputs[1]) {
            nativeDirectRRComposeFailure = "Native Direct RR preview inputs are unavailable";
            return false;
        }
        try {
            auto* device = backend.d3d12Device.get();
            auto* list = frame.commands.get();
            if (!nativeDirectRRComposeRoot || !nativeDirectRRComposePipeline) {
                std::ifstream file(
                    "Data/Shaders/RTDirectLighting/nativeDirectRRCompose.dxil",
                    std::ios::binary | std::ios::ate);
                if (!file) throw std::runtime_error("Native Direct RR composition DXIL missing");
                const auto byteSize = file.tellg();
                if (byteSize <= 0 || byteSize > 16 * 1024 * 1024)
                    throw std::runtime_error("Invalid native Direct RR composition DXIL");
                std::vector<char> code(static_cast<size_t>(byteSize));
                file.seekg(0);
                if (!file.read(code.data(), std::streamsize(code.size())))
                    throw std::runtime_error("Cannot read native Direct RR composition DXIL");
                D3D12_DESCRIPTOR_RANGE ranges[2]{};
                ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0 };
                ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
                D3D12_ROOT_PARAMETER params[3]{};
                for (uint32_t i = 0; i < 2u; ++i) {
                    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                    params[i].DescriptorTable = { 1, &ranges[i] };
                }
                params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                params[2].Constants = { 0, 0, 4 };
                const D3D12_ROOT_SIGNATURE_DESC signature{
                    3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
                winrt::com_ptr<ID3DBlob> serialized, errors;
                DX::ThrowIfFailed(D3D12SerializeRootSignature(&signature,
                    D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put()));
                DX::ThrowIfFailed(device->CreateRootSignature(0,
                    serialized->GetBufferPointer(), serialized->GetBufferSize(),
                    IID_PPV_ARGS(nativeDirectRRComposeRoot.put())));
                D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
                state.pRootSignature = nativeDirectRRComposeRoot.get();
                state.CS = { code.data(), code.size() };
                DX::ThrowIfFailed(device->CreateComputePipelineState(
                    &state, IID_PPV_ARGS(nativeDirectRRComposePipeline.put())));
            }
            if (!frame.nativeDirectRRScene) {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = frame.size[0]; desc.Height = frame.size[1];
                desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                frame.nativeDirectRRScene = std::make_unique<WrappedResource>(desc,
                    backend.d3d11Device.get(), device,
                    "RTComposer::NativeDirectRRComposedScene");
            }
            if (!frame.nativeDirectRRComposeDescriptors) {
                const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 9u,
                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
                DX::ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc,
                    IID_PPV_ARGS(frame.nativeDirectRRComposeDescriptors.put())));
            }
            const std::array<ID3D12Resource*, 8> reads{
                frame.hybridShadowedSource.get(),
                frame.nativeDirectRROutputs[0].get(),
                frame.nativeDirectRROutputs[1].get(),
                frame.directSignals[4]->resource.get(),
                frame.directSignals[3]->resource.get(),
                frame.output->resource.get(),
                frame.directSignals[0]->resource.get(),
                frame.directSignals[1]->resource.get()
            };
            const UINT step = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto cpu = frame.nativeDirectRRComposeDescriptors->GetCPUDescriptorHandleForHeapStart();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            for (uint32_t i = 0; i < reads.size(); ++i) {
                srv.Format = i == 5u ? DXGI_FORMAT_R32G32B32A32_FLOAT :
                    DXGI_FORMAT_R16G16B16A16_FLOAT;
                device->CreateShaderResourceView(reads[i], &srv, cpu);
                cpu.ptr += step;
            }
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(
                frame.nativeDirectRRScene->resource.get(), nullptr, &uav, cpu);
            std::array<D3D12_RESOURCE_BARRIER, 9> before{}, after{};
            for (uint32_t i = 0; i < reads.size(); ++i) {
                before[i] = CD3DX12_RESOURCE_BARRIER::Transition(reads[i],
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                after[i] = CD3DX12_RESOURCE_BARRIER::Transition(reads[i],
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COMMON);
            }
            before[8] = CD3DX12_RESOURCE_BARRIER::Transition(
                frame.nativeDirectRRScene->resource.get(),
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            after[8] = CD3DX12_RESOURCE_BARRIER::Transition(
                frame.nativeDirectRRScene->resource.get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            list->ResourceBarrier(UINT(before.size()), before.data());
            ID3D12DescriptorHeap* heaps[]{ frame.nativeDirectRRComposeDescriptors.get() };
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(nativeDirectRRComposeRoot.get());
            list->SetPipelineState(nativeDirectRRComposePipeline.get());
            auto gpu = frame.nativeDirectRRComposeDescriptors->GetGPUDescriptorHandleForHeapStart();
            list->SetComputeRootDescriptorTable(0, gpu);
            gpu.ptr += uint64_t(8u) * step;
            list->SetComputeRootDescriptorTable(1, gpu);
            const float blend = std::clamp(composer.developer.nativeDirectRRPreviewBlend, 0.f, 1.f);
            const uint32_t constants[4]{
                frame.size[0], frame.size[1], std::bit_cast<uint32_t>(blend), 0u };
            list->SetComputeRoot32BitConstants(2, 4, constants, 0);
            list->Dispatch((frame.size[0] + 7u) / 8u,
                (frame.size[1] + 7u) / 8u, 1u);
            list->ResourceBarrier(UINT(after.size()), after.data());
            frame.nativeDirectRRComposeRootRef = nativeDirectRRComposeRoot;
            frame.nativeDirectRRComposePipelineRef = nativeDirectRRComposePipeline;
            frame.nativeDirectRRComposed = true;
            ++nativeDirectRRComposeDispatches;
            nativeDirectRRComposeFailure.clear();
            return true;
        } catch (const std::exception& error) {
            nativeDirectRRComposeFailure =
                std::string("Native Direct RR preview: ") + error.what();
            return false;
        }
    }

    bool DirectionalTrace::RecordFSRIndependentGI(Frame& frame)
    {
        frame.nativeGI.valid = frame.nativeGI.rrValid = false;
        frame.nativeGI.postValid = frame.nativeGI.composedValid = false;
        frame.nativeGI.rrFamilySuccessMask = 0u;
        if (!frame.fsrIndependent || !frame.nativeGIRequested ||
            !frame.fsrDepthReady || !frame.hybridInputsReady ||
            !frame.hybridInputs[0] || !frame.hybridInputs[2] ||
            !frame.hybridInputs[3] || !frame.hybridInputs[4] ||
            !frame.hybridInputs[5]) return false;
        try {
            const auto& settings = globals::features::rtComposer.composition.fsrHybrid;
            const auto sampling = MakeHybridSamplingLayout(
                settings.resolution, settings.checkerboard, frame.size, frame.number);
            if (!sampling.logical[0] || !sampling.logical[1] ||
                !sampling.packed[0] || !sampling.packed[1] ||
                (sampling.checkerboard && (sampling.logical[0] & 1u))) {
                fsrIndependentGIFailure = "Invalid native FSR HybridGI signal layout";
                ++fsrIndependentGIFailures;
                return false;
            }
            if (!fsrIndependentGI)
                fsrIndependentGI = std::make_unique<NativeHybridGI>();
            auto* device = backend.d3d12Device.get();
            auto* list = frame.commands.get();
            const uint32_t slot = uint32_t(&frame - frames.data());
            if (!fsrIndependentGI->Record(device, list, frame.nativeGI, slot,
                    frame.number, frame.size, sampling,
                    frame.hybridLinearDepth.get(),
                    frame.hybridInputs[2]->resource.get(),
                    frame.hybridInputs[0]->resource.get(),
                    frame.hybridInputs[3]->resource.get(),
                    frame.hybridInputs[4]->resource.get(), nullptr,
                    globals::features::redstoneGI, 0.0f, 0.0f, true, true)) {
                fsrIndependentGIFailure = fsrIndependentGI->Failure();
                ++fsrIndependentGIFailures;
                return false;
            }
            frame.nativeGIFrame = frame.number;
            // AMD is an optional consumer of raw GI/AO, not a requirement for
            // producing or displaying the underlying lighting signals.
            if (!globals::features::rtComposer.composition.useFSRRayRegeneration)
                return true;
            if (!fsrIndependentGI->RecordRRGuides(device, list, frame.nativeGI,
                    slot, frame.hybridLinearDepth.get(),
                    frame.hybridInputs[2]->resource.get(),
                    frame.hybridInputs[5]->resource.get(),
                    frame.hybridInputs[3]->resource.get(),
                    frame.hybridInputs[4]->resource.get())) {
                fsrIndependentGIFailure = fsrIndependentGI->GuideFailure();
                ++fsrIndependentGIFailures;
                return true; // raw GI remains available
            }
            // No RecordHistoryValidation, RecordPostRR, native spatial,
            // temporal reconstruction, post-RR blur or legacy fallback.
            if (!fsrHybridRRDenoiser)
                fsrHybridRRDenoiser = std::make_unique<RedstoneDenoiser>();
            if (!fsrHybridRRDenoiser->InitializeNative(backend)) {
                fsrIndependentGIFailure = "Independent AMD HybridGI RR unavailable";
                ++fsrIndependentGIFailures;
                return true; // raw GI remains available
            }
            auto& outputs = frame.nativeGI;
            if (outputs.rrOutputSize != sampling.logical) {
                for (auto& resource : outputs.rrOutputs) resource = nullptr;
                outputs.rrOutputSize = sampling.logical;
            }
            for (uint32_t i = 0; i < outputs.rrOutputs.size(); ++i) {
                if (outputs.rrOutputs[i]) continue;
                const DXGI_FORMAT format = i < 2u ?
                    DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8_UNORM;
                const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format,
                    sampling.logical[0], sampling.logical[1], 1, 1, 1, 0,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
                DX::ThrowIfFailed(device->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                    IID_PPV_ARGS(outputs.rrOutputs[i].put())));
            }
            RedstoneDenoiser::NativeHybridResources resources{};
            resources.nativeSize = frame.size;
            resources.logicalSize = sampling.logical;
            resources.checkerboard = sampling.checkerboard;
            resources.checkerboardOrigin = sampling.checkerboardOrigin;
            resources.frame = frame.number;
            resources.resetHistory = false; // AMD alone owns its temporal history
            resources.directFamilies = false;
            resources.skipOcclusion = false;
            resources.inputs[0] = outputs.textures[NativeHybridGI::Diffuse].get();
            resources.inputs[1] = outputs.textures[NativeHybridGI::Specular].get();
            resources.inputs[2] = outputs.textures[NativeHybridGI::DiffuseAO].get();
            resources.inputs[3] = outputs.textures[NativeHybridGI::SpecularAO].get();
            for (uint32_t i = 0; i < 4u; ++i)
                resources.outputs[i] = outputs.rrOutputs[i].get();
            for (uint32_t i = 0; i < resources.guides.size(); ++i)
                resources.guides[i] = outputs.rrGuides[i].get();
            outputs.rrValid = fsrHybridRRDenoiser->DispatchNativeHybridRR(list, resources);
            outputs.rrFamilySuccessMask =
                fsrHybridRRDenoiser->NativeHybridSuccessfulFamilyMask();
            if (!outputs.rrValid) {
                // Successful sibling RR families are still composable;
                // missing families use their original raw GPU signals.
                fsrIndependentGIFailure = fsrHybridRRDenoiser->NativeHybridFailure();
                ++fsrIndependentGIFailures;
                return true;
            }
            fsrIndependentGIFailure.clear();
            ++fsrIndependentGISuccesses;
            return true;
        } catch (const std::exception& error) {
            fsrIndependentGIFailure = error.what();
            ++fsrIndependentGIFailures;
            frame.nativeGI.rrValid = false;
            return false;
        }
    }

    bool DirectionalTrace::RecordFSRIndependentComposition(Frame& frame)
    {
        frame.fsrIndependentComposed = false;
        if (!frame.fsrIndependent || !frame.hybridInputsReady ||
            !frame.hybridInputs[0] || !frame.hybridInputs[2] ||
            !frame.hybridInputs[3] || !frame.hybridInputs[4])
            return false;
        try {
            auto* device = backend.d3d12Device.get();
            auto* list = frame.commands.get();
            if (!fsrIndependentComposeRoot || !fsrIndependentComposePipeline) {
                std::ifstream file(
                    "Data/Shaders/RTDirectLighting/fsrIndependentCompose.dxil",
                    std::ios::binary | std::ios::ate);
                if (!file) throw std::runtime_error("FSR Independent compositor DXIL missing");
                const auto length = file.tellg();
                if (length <= 0 || length > 16 * 1024 * 1024)
                    throw std::runtime_error("Invalid FSR Independent compositor DXIL");
                std::vector<char> code(static_cast<size_t>(length));
                file.seekg(0);
                if (!file.read(code.data(), std::streamsize(code.size())))
                    throw std::runtime_error("Cannot read FSR Independent compositor DXIL");
                D3D12_DESCRIPTOR_RANGE ranges[2]{};
                ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 17u, 0, 0, 0 };
                ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1u, 0, 0, 0 };
                D3D12_ROOT_PARAMETER params[3]{};
                for (uint32_t i = 0; i < 2u; ++i) {
                    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                    params[i].DescriptorTable = { 1, &ranges[i] };
                }
                params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                params[2].Constants = { 0, 0, 12u };
                const D3D12_ROOT_SIGNATURE_DESC signature{
                    3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
                winrt::com_ptr<ID3DBlob> serialized, errors;
                DX::ThrowIfFailed(D3D12SerializeRootSignature(
                    &signature, D3D_ROOT_SIGNATURE_VERSION_1,
                    serialized.put(), errors.put()));
                DX::ThrowIfFailed(device->CreateRootSignature(
                    0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                    IID_PPV_ARGS(fsrIndependentComposeRoot.put())));
                D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
                ps.pRootSignature = fsrIndependentComposeRoot.get();
                ps.CS = { code.data(), code.size() };
                DX::ThrowIfFailed(device->CreateComputePipelineState(
                    &ps, IID_PPV_ARGS(fsrIndependentComposePipeline.put())));
            }
            if (!frame.fsrIndependentScene) {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = frame.size[0]; desc.Height = frame.size[1];
                desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                frame.fsrIndependentScene = std::make_unique<WrappedResource>(
                    desc, backend.d3d11Device.get(), device, "FSR::IndependentScene");
            }
            if (!frame.fsrIndependentComposeDescriptors) {
                const D3D12_DESCRIPTOR_HEAP_DESC desc{
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 18u,
                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
                DX::ThrowIfFailed(device->CreateDescriptorHeap(
                    &desc, IID_PPV_ARGS(frame.fsrIndependentComposeDescriptors.put())));
            }
            const auto& gi = frame.nativeGI;
            const bool hybridAvailable = gi.valid &&
                gi.textures[NativeHybridGI::Diffuse] &&
                gi.textures[NativeHybridGI::Specular] &&
                gi.textures[NativeHybridGI::DiffuseAO] &&
                gi.textures[NativeHybridGI::SpecularAO];
            // Null Direct SRVs read as zero. HybridGI thus works without a
            // prepared DXR scene, while complete Direct RR is still composed
            // from its original signals when that producer is available.
            const bool directAvailable = frame.output &&
                frame.directSignals[0] && frame.directSignals[1] &&
                frame.directSignals[3] && frame.directSignals[4];
            if (!hybridAvailable && !directAvailable) return false;
            uint32_t directRRMask = frame.nativeDirectRRSuccessMask & 3u;
            if (!frame.nativeDirectRROutputs[0]) directRRMask &= ~1u;
            if (!frame.nativeDirectRROutputs[1]) directRRMask &= ~2u;
            uint32_t hybridRRMask = hybridAvailable ?
                (gi.rrFamilySuccessMask & 7u) : 0u;
            if (!gi.rrOutputs[0]) hybridRRMask &= ~1u;
            if (!gi.rrOutputs[1]) hybridRRMask &= ~2u;
            if (!gi.rrOutputs[2] || !gi.rrOutputs[3]) hybridRRMask &= ~4u;
            const std::array<ID3D12Resource*, 17> inputs{
                frame.hybridInputs[0]->resource.get(),
                directAvailable ? frame.output->resource.get() : nullptr,
                directAvailable && (directRRMask & 1u) ?
                    frame.nativeDirectRROutputs[0].get() : nullptr,
                directAvailable && (directRRMask & 2u) ?
                    frame.nativeDirectRROutputs[1].get() : nullptr,
                directAvailable ? frame.directSignals[4]->resource.get() : nullptr,
                directAvailable ? frame.directSignals[3]->resource.get() : nullptr,
                !hybridAvailable ? nullptr : (hybridRRMask & 1u) ?
                    gi.rrOutputs[0].get() : gi.textures[NativeHybridGI::Diffuse].get(),
                !hybridAvailable ? nullptr : (hybridRRMask & 2u) ?
                    gi.rrOutputs[1].get() : gi.textures[NativeHybridGI::Specular].get(),
                !hybridAvailable ? nullptr : (hybridRRMask & 4u) ?
                    gi.rrOutputs[2].get() : gi.textures[NativeHybridGI::DiffuseAO].get(),
                !hybridAvailable ? nullptr : (hybridRRMask & 4u) ?
                    gi.rrOutputs[3].get() : gi.textures[NativeHybridGI::SpecularAO].get(),
                frame.hybridInputs[3]->resource.get(),
                frame.hybridInputs[4]->resource.get(),
                frame.hybridLinearDepth.get(),
                frame.hybridInputs[2]->resource.get(),
                directAvailable && frame.receivers ? frame.receivers->resource.get() : nullptr,
                directAvailable ? frame.directSignals[0]->resource.get() : nullptr,
                directAvailable ? frame.directSignals[1]->resource.get() : nullptr
            };
            const std::array<DXGI_FORMAT, 17> formats{
                DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM,
                DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT
            };
            std::vector<D3D12_RESOURCE_BARRIER> before, after;
            before.reserve(18);
            after.reserve(18);
            auto cpu = frame.fsrIndependentComposeDescriptors->GetCPUDescriptorHandleForHeapStart();
            const UINT step = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            for (uint32_t i = 0; i < inputs.size(); ++i) {
                srv.Format = formats[i];
                device->CreateShaderResourceView(inputs[i], &srv, cpu);
                cpu.ptr += step;
                if (inputs[i]) {
                    before.push_back(CD3DX12_RESOURCE_BARRIER::Transition(inputs[i],
                        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
                    after.push_back(CD3DX12_RESOURCE_BARRIER::Transition(inputs[i],
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
                }
            }
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(
                frame.fsrIndependentScene->resource.get(), nullptr, &uav, cpu);
            before.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                frame.fsrIndependentScene->resource.get(),
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
            after.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                frame.fsrIndependentScene->resource.get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
            list->ResourceBarrier(UINT(before.size()), before.data());
            list->SetComputeRootSignature(fsrIndependentComposeRoot.get());
            list->SetPipelineState(fsrIndependentComposePipeline.get());
            ID3D12DescriptorHeap* heaps[]{ frame.fsrIndependentComposeDescriptors.get() };
            list->SetDescriptorHeaps(1, heaps);
            auto gpu = frame.fsrIndependentComposeDescriptors->GetGPUDescriptorHandleForHeapStart();
            list->SetComputeRootDescriptorTable(0, gpu);
            gpu.ptr += 17u * step;
            list->SetComputeRootDescriptorTable(1, gpu);
            const auto& policy = globals::features::rtComposer.composition.fsrHybrid;
            const std::array<uint32_t, 2> logical =
                hybridAvailable ? gi.sampling.logical : std::array<uint32_t, 2>{};
            const uint32_t constants[12]{
                frame.size[0], frame.size[1], logical[0], logical[1],
                frame.number,
                uint32_t(globals::features::linearLighting.IsLinearLightingActive()),
                std::bit_cast<uint32_t>(policy.rasterDirectBlend),
                std::bit_cast<uint32_t>(policy.contribution),
                directAvailable ? directRRMask : 0u, hybridRRMask,
                hybridAvailable ? (uint32_t(gi.sampling.checkerboard) |
                    ((gi.sampling.checkerboardOrigin & 1u) << 1u)) : 0u, 0u };
            list->SetComputeRoot32BitConstants(2, 12, constants, 0);
            list->Dispatch((frame.size[0] + 7u) / 8u,
                (frame.size[1] + 7u) / 8u, 1u);
            list->ResourceBarrier(UINT(after.size()), after.data());
            frame.fsrIndependentComposeRootRef = fsrIndependentComposeRoot;
            frame.fsrIndependentComposePipelineRef = fsrIndependentComposePipeline;
            frame.fsrIndependentComposed = true;
            ++fsrIndependentComposeDispatches;
            fsrIndependentComposeFailure.clear();
            return true;
        } catch (const std::exception& error) {
            fsrIndependentComposeFailure = error.what();
            return false;
        }
    }

    void DirectionalTrace::RecordNativeHybridGI(Frame& frame)
    {
        if (globals::features::rtComposer.AdvancedActive()) {
            RecordAdvancedTransport(frame);
            return;
        }
        frame.nativeGI.valid = false;
        frame.nativeGI.composedValid = false;
        if (!frame.nativeGIRequested || !frame.hybridShadowedSourceReady ||
            !frame.hybridLinearDepth || !frame.hybridShadowedSource || !frame.shadowMask ||
            !frame.hybridInputs[0] ||
            !frame.hybridInputs[2] || !frame.hybridInputs[3] || !frame.hybridInputs[4] ||
            !frame.hybridInputs[5]) return;
        if (!nativeHybridGI) nativeHybridGI = std::make_unique<NativeHybridGI>();
        const auto& composer = globals::features::rtComposer;
        const auto& composition = composer.composition;
        const auto tier = RTComposer::GetTierPolicy(RTComposer::CompositorMode::HybridShadowed);
        const bool research = composer.LegacyHybridPrototypeActive();
        const auto& researchSettings = composition.fsrHybrid;
        const bool rasterScene = composer.UseRasterHybridGISource();
        // Change only the raw scene radiance sampled by HybridGI.
        // The separate shadowed scene still supplies final composition.
        ID3D12Resource* hybridRadiance = rasterScene ?
            frame.hybridInputs[0]->resource.get() :
            frame.hybridShadowedSource.get();
        const auto sampling = MakeHybridSamplingLayout(
            research ? researchSettings.resolution :
                (composer.IsFixedPreset() ? tier.hybridResolution : composition.hybridResolution),
            research ? researchSettings.checkerboard :
                (composer.IsFixedPreset() ? tier.hybridCheckerboard : composition.hybridCheckerboard),
            frame.size, frame.number);
        if (nativeHybridGI->Record(backend.d3d12Device.get(), frame.commands.get(),
                frame.nativeGI, uint32_t(&frame - frames.data()), frame.number, frame.size,
                sampling, frame.hybridLinearDepth.get(), frame.hybridInputs[2]->resource.get(),
                hybridRadiance, frame.hybridInputs[3]->resource.get(),
                frame.hybridInputs[4]->resource.get(), EffectiveShadowMask(frame),
                globals::features::redstoneGI,
                research ? researchSettings.shadowInfluence :
                    (composer.IsFixedPreset() ? tier.shadowInfluence : composition.shadowInfluence),
                research ? researchSettings.shadowSkipThreshold :
                    (composer.IsFixedPreset() ? 0.25f : composition.shadowThreshold),
                rasterScene)) {
            frame.nativeGIFrame = frame.number;
            hybridGIInputScene = rasterScene ? "raster-baseline" : "native-shadowed";
            hybridGIInputSceneFrame = frame.number;
            // Native RR guides are GPU-only and allocated at the *logical*
            // RR grid. They are not sent through the legacy DX11 denoiser.
            const bool guidesReady = nativeHybridGI->RecordRRGuides(
                backend.d3d12Device.get(), frame.commands.get(),
                frame.nativeGI, uint32_t(&frame - frames.data()),
                frame.hybridLinearDepth.get(),
                frame.hybridInputs[2]->resource.get(),
                frame.hybridInputs[5]->resource.get(),
                frame.hybridInputs[3]->resource.get(),
                frame.hybridInputs[4]->resource.get());
            // The native RR provider owns persistent previous-frame depth
            // and normal snapshots; recyclable tracer slots never do.
            if (guidesReady)
                nativeHybridGI->RecordHistoryValidation(
                    backend.d3d12Device.get(), frame.commands.get(), frame.nativeGI,
                    uint32_t(&frame - frames.data()), frame.number,
                    frame.generation ^ (composer.developerRevision * 0x9e3779b97f4a7c15ULL));
            // Native Hybrid-Shadowed is a production path. The completed DX12
            // scene is the only product returned to D3D11, not legacy GI signals.
            // Native DX12 HybridGI keeps its shadow-adjusted producer
            // regardless of the reconstruction provider selected for its signals.
            const bool rrRequested = composer.IsFSRRenderer() &&
                composer.DX12HybridActive() &&
                (composer.IsFixedPreset() ?
                    composition.fixedRR.Get(composition.mode, 2u) :
                    (composition.useFSRRayRegeneration &&
                     RayRegeneration::GetSettings().enabled));
            RedstoneDenoiser* provider = nullptr;
            if (rrRequested) {
                auto& candidate = globals::features::redstoneGI.GetDenoiser();
                if (candidate.Initialize(backend)) provider = &candidate;
            }
            auto activeReconstruction = composer.EffectiveReconstructionPolicy();
            const bool reconstructed = guidesReady &&
                nativeHybridGI->RecordPostRR(
                    backend.d3d12Device.get(), frame.commands.get(),
                    frame.nativeGI, uint32_t(&frame - frames.data()),
                    frame.number, frame.hybridLinearDepth.get(),
                    frame.hybridInputs[2]->resource.get(), activeReconstruction,
                    provider, rrRequested);
            // A genuine full-native producer can compose its raw signals if
            // a reconstruction shader is unavailable; sparse signals cannot.
            const bool rawNative = !reconstructed &&
                sampling.effective == HybridSamplingMode::Native;
            if (reconstructed || rawNative)
                nativeHybridGI->RecordComposition(backend.d3d12Device.get(),
                    backend.d3d11Device.get(), frame.commands.get(), frame.nativeGI,
                    uint32_t(&frame - frames.data()), frame.size,
                    frame.hybridShadowedSource.get(), frame.output->resource.get(),
                    EffectiveShadowMask(frame), frame.hybridLinearDepth.get(),
                    frame.hybridInputs[3]->resource.get(),
                    frame.hybridInputs[4]->resource.get(),
                    frame.rasterBlend * (globals::features::rtComposer.IsFixedPreset() ?
                        RTComposer::GetTierPolicy(RTComposer::CompositorMode::HybridShadowed).directContribution :
                        composition.rtDirectContribution),
                    research ? researchSettings.contribution :
                        (composer.IsFixedPreset() ? 1.0f : composition.giContribution),
                    composer.IsFixedPreset() ? 1u : 0u,
                    research ? researchSettings.shadowInfluence :
                        (composer.IsFixedPreset() ? tier.shadowInfluence : composition.shadowInfluence));
        }
    }

    void DirectionalTrace::RecordAdvancedTransport(Frame& frame)
    {
        frame.nativeGI.valid = frame.nativeGI.composedValid = false;
        frame.postHybridGI.valid = frame.postHybridGI.composedValid = false;
        advancedComposeFailure.clear();
        if (!frame.nativeGIRequested || !frame.hybridShadowedSourceReady ||
            !frame.hybridLinearDepth || !frame.hybridShadowedSource ||
            !frame.hybridInputs[2] || !frame.hybridInputs[3] ||
            !frame.hybridInputs[4] || !frame.hybridInputs[5] ||
            !frame.directSignals[5] || !frame.directSignals[6] ||
            frame.transportTier < 2u || frame.transportTier > 5u) return;

        if (!nativeHybridGI) nativeHybridGI = std::make_unique<NativeHybridGI>();
        const auto& composer = globals::features::rtComposer;
        const auto policy = RTComposer::GetTierPolicy(composer.composition.mode);
        // Fixed RR modes must never silently inherit Custom filter and blur
        // overrides, which affect both quality and memory requirements.
        auto activeReconstruction = composer.EffectiveReconstructionPolicy();
        const uint32_t slot = uint32_t(&frame - frames.data());
        if (!nativeHybridGI->RecordAdvancedTransport(
                backend.d3d12Device.get(), frame.commands.get(), frame.nativeGI,
                slot, frame.number, frame.size, frame.transportTier,
                frame.directSignals[5]->resource.get(),
                frame.directSignals[6]->resource.get(),
                frame.receivers->resource.get(), frame.hybridLinearDepth.get())) {
            advancedComposeFailure = nativeHybridGI->AdvancedFailure();
            return;
        }
        frame.nativeGIFrame = frame.number;
        if (!nativeHybridGI->RecordRRGuides(
                backend.d3d12Device.get(), frame.commands.get(), frame.nativeGI,
                slot, frame.hybridLinearDepth.get(),
                frame.hybridInputs[2]->resource.get(),
                frame.hybridInputs[5]->resource.get(),
                frame.hybridInputs[3]->resource.get(),
                frame.hybridInputs[4]->resource.get())) {
            advancedComposeFailure = nativeHybridGI->GuideFailure();
            return;
        }
        nativeHybridGI->RecordHistoryValidation(
            backend.d3d12Device.get(), frame.commands.get(), frame.nativeGI,
            slot, frame.number, frame.generation);
        // First-pass Indirect RR is optional, with a native DX12 fallback.
        const bool indirectRR = composer.IsFSRRenderer() &&
            (composer.IsFixedPreset() ?
                composer.composition.fixedRR.Get(composer.composition.mode, 1u) :
                (composer.composition.useFSRRayRegeneration &&
                 RayRegeneration::GetSettings().enabled));
        RedstoneDenoiser* firstProvider = nullptr;
        if (indirectRR) {
            if (!advancedDenoiser)
                advancedDenoiser = std::make_unique<RedstoneDenoiser>();
            if (advancedDenoiser->Initialize(backend))
                firstProvider = advancedDenoiser.get();
        }
        if (!nativeHybridGI->RecordPostRR(
                backend.d3d12Device.get(), frame.commands.get(),
                frame.nativeGI, slot, frame.number, frame.hybridLinearDepth.get(),
                frame.hybridInputs[2]->resource.get(), activeReconstruction,
                firstProvider, indirectRR)) {
            advancedComposeFailure = nativeHybridGI->PostFailure();
            return;
        }
        if (!nativeHybridGI->RecordComposition(
                backend.d3d12Device.get(), backend.d3d11Device.get(),
                frame.commands.get(), frame.nativeGI, slot, frame.size,
                frame.hybridShadowedSource.get(), frame.output->resource.get(),
                EffectiveShadowMask(frame), frame.hybridLinearDepth.get(),
                frame.hybridInputs[3]->resource.get(),
                frame.hybridInputs[4]->resource.get(),
                frame.rasterBlend * policy.directContribution, 1.0f, 0u,
                policy.shadowInfluence, frame.transportTier)) {
            advancedComposeFailure = nativeHybridGI->CompositionFailure();
            return;
        }
        ++advancedComposeSubmissions;

        if (frame.transportTier < 4u) return;
        // Enhanced tiers: follow the already composed linear RT scene with a
        // SEPARATE quarter-size checkerboard HybridGI. Never feed the original
        // raster into this second pass and never gamma-encode the input twice.
        if (!postHybridGI) postHybridGI = std::make_unique<NativeHybridGI>();
        const auto sampling = MakeHybridSamplingLayout(2u, true, frame.size, frame.number);
        if (!sampling.checkerboard ||
            !postHybridGI->Record(backend.d3d12Device.get(),
                frame.commands.get(), frame.postHybridGI, slot, frame.number,
                frame.size, sampling, frame.hybridLinearDepth.get(),
                frame.hybridInputs[2]->resource.get(),
                frame.nativeGI.composedScene->resource.get(),
                frame.hybridInputs[3]->resource.get(),
                frame.hybridInputs[4]->resource.get(),
                EffectiveShadowMask(frame), globals::features::redstoneGI,
                policy.shadowInfluence, composer.composition.shadowThreshold)) {
            advancedComposeFailure = postHybridGI->Failure();
            return;
        }
        if (!postHybridGI->RecordRRGuides(
                backend.d3d12Device.get(), frame.commands.get(),
                frame.postHybridGI, slot, frame.hybridLinearDepth.get(),
                frame.hybridInputs[2]->resource.get(),
                frame.hybridInputs[5]->resource.get(),
                frame.hybridInputs[3]->resource.get(),
                frame.hybridInputs[4]->resource.get())) {
            advancedComposeFailure = postHybridGI->GuideFailure();
            return;
        }
        postHybridGI->RecordHistoryValidation(
            backend.d3d12Device.get(), frame.commands.get(), frame.postHybridGI,
            slot, frame.number, frame.generation);
        // Post-composite HybridGI RR is a separate reconstruction request.
        const bool hybridRR = composer.IsFSRRenderer() &&
            composer.composition.fixedRR.Get(composer.composition.mode, 2u);
        RedstoneDenoiser* postProvider = nullptr;
        if (hybridRR) {
            auto& candidate = globals::features::redstoneGI.GetDenoiser();
            if (candidate.Initialize(backend)) postProvider = &candidate;
        }
        if (!postHybridGI->RecordPostRR(
                backend.d3d12Device.get(), frame.commands.get(),
                frame.postHybridGI, slot, frame.number, frame.hybridLinearDepth.get(),
                frame.hybridInputs[2]->resource.get(), activeReconstruction,
                postProvider, hybridRR)) {
            advancedComposeFailure = postHybridGI->PostFailure();
            return;
        }
        if (!postHybridGI->RecordComposition(
                backend.d3d12Device.get(), backend.d3d11Device.get(),
                frame.commands.get(), frame.postHybridGI, slot, frame.size,
                frame.nativeGI.composedScene->resource.get(),
                frame.output->resource.get(), EffectiveShadowMask(frame),
                frame.hybridLinearDepth.get(),
                frame.hybridInputs[3]->resource.get(),
                frame.hybridInputs[4]->resource.get(),
                0.0f, 1.0f, 0u, policy.shadowInfluence)) {
            advancedComposeFailure = postHybridGI->CompositionFailure();
            return;
        }
        ++postHybridComposeSubmissions;
    }

	void DirectionalTrace::Record(Frame& a_frame, const float3& a_direction, bool a_localLight)
	{
		CS_GPU_PASS("RTDirectLighting::DirectionalTraversal");
		a_frame.root = root;
		a_frame.pipeline = pipeline;
		if (a_frame.allocator) DX::ThrowIfFailed(a_frame.allocator->Reset());
		else DX::ThrowIfFailed(backend.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(a_frame.allocator.put())));
		if (a_frame.commands) DX::ThrowIfFailed(a_frame.commands->Reset(
            a_frame.allocator.get(), a_frame.fsrHybridOnly ? nullptr : pipeline.get()));
        else DX::ThrowIfFailed(backend.d3d12Device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, a_frame.allocator.get(),
            a_frame.fsrHybridOnly ? nullptr : pipeline.get(),
            IID_PPV_ARGS(a_frame.commands.put())));
		// Independent raster GI/AO starts as soon as captured DX11 source inputs
        // become GPU-visible; it does not depend on Direct or shadow work.
        if (a_frame.fsrIndependent && a_frame.hybridInputsReady &&
            RecordFSRRasterDepth(a_frame))
            (void)RecordFSRIndependentGI(a_frame);
        if (a_frame.fsrHybridOnly) {
            // Direct ray traversal, its signal packing, and its RR processing
            // are unchanged and never executed in the GI-only case.
            if (a_frame.nativeGI.valid)
                (void)RecordFSRIndependentComposition(a_frame);
            DX::ThrowIfFailed(a_frame.commands->Close());
            ID3D12CommandList* lists[]{ a_frame.commands.get() };
            backend.commandQueue->ExecuteCommandLists(1, lists);
            a_frame.fence = ++nextCompletion;
            DX::ThrowIfFailed(backend.commandQueue->Signal(
                completion.fence12.get(), a_frame.fence));
            ++submissions;
            return;
        }
        // Early FSR HybridGI used a different PSO. Restore the DXR Direct PSO.
        a_frame.commands->SetPipelineState(pipeline.get());
        const auto step = backend.d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = a_frame.bindings->heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += uint64_t(a_frame.descriptorBase) * step;
		D3D12_SHADER_RESOURCE_VIEW_DESC receiver{};
		receiver.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		receiver.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		receiver.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		receiver.Texture2D.MipLevels = 1;
		backend.d3d12Device->CreateShaderResourceView(a_frame.receivers->resource.get(), &receiver, cpu);
		cpu.ptr += step;
		D3D12_UNORDERED_ACCESS_VIEW_DESC output{};
		output.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		output.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		backend.d3d12Device->CreateUnorderedAccessView(a_frame.output->resource.get(), nullptr, &output, cpu);
		cpu.ptr += step;
		output.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		for (const auto& signal : a_frame.directSignals) {
			backend.d3d12Device->CreateUnorderedAccessView(signal ? signal->resource.get() : nullptr, nullptr, &output, cpu);
			cpu.ptr += step;
		}
		output.Format = DXGI_FORMAT_R32G32_UINT;
		backend.d3d12Device->CreateUnorderedAccessView(a_frame.visibilityBits ? a_frame.visibilityBits.get() : nullptr, nullptr, &output, cpu);
		cpu.ptr += step;
		output.Format = DXGI_FORMAT_R16G16_FLOAT;
		backend.d3d12Device->CreateUnorderedAccessView(a_frame.shadowMask ? a_frame.shadowMask.get() : nullptr, nullptr, &output, cpu);
		cpu.ptr += step;
		output.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		backend.d3d12Device->CreateUnorderedAccessView(a_frame.shadowMaskExport ? a_frame.shadowMaskExport->resource.get() : nullptr, nullptr, &output, cpu);
		cpu.ptr += step;
		backend.d3d12Device->CreateShaderResourceView(a_frame.light->resource.get(), &receiver, cpu);
		ID3D12DescriptorHeap* heaps[]{ a_frame.bindings->heap.get() };
		auto* commands = a_frame.commands.get();
		commands->SetDescriptorHeaps(1, heaps);
		commands->SetComputeRootSignature(root.get());
		commands->SetComputeRootShaderResourceView(0, a_frame.scene->result->GetGPUVirtualAddress());
		auto gpu = a_frame.bindings->heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += uint64_t(a_frame.descriptorBase) * step;
		commands->SetComputeRootDescriptorTable(1, gpu);
		gpu.ptr += step; commands->SetComputeRootDescriptorTable(2, gpu);
		gpu.ptr += uint64_t(1 + kDirectSignalCount + kShadowSignalCount) * step; commands->SetComputeRootDescriptorTable(6, gpu);
		gpu = a_frame.bindings->heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += uint64_t(kGeometryDescriptorBase) * step;
		commands->SetComputeRootDescriptorTable(3, gpu);
		commands->SetComputeRootShaderResourceView(4, a_frame.bindings->offsets->GetGPUVirtualAddress());
		gpu = a_frame.bindings->heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += uint64_t(kAttributeDescriptorBase) * step;
		commands->SetComputeRootDescriptorTable(7, gpu);
		gpu = a_frame.bindings->heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += uint64_t(kTextureDescriptorBase) * step;
		commands->SetComputeRootDescriptorTable(8, gpu);
		commands->SetComputeRootShaderResourceView(9, a_frame.bindings->materials->surfaces->GetGPUVirtualAddress());
		commands->SetComputeRootShaderResourceView(10, a_frame.bindings->materials->materials->GetGPUVirtualAddress());
		commands->SetComputeRootShaderResourceView(11, a_frame.bindings->materials->roomVolumes->GetGPUVirtualAddress());
		struct Constants
		{
			uint32_t width, height, frame, count;
			float3 direction;
			float maximum;
			float3 origin;
			float minimum, radius;
			uint32_t padding[3]{};
			float4 strengths;
			uint32_t exportSignals, signalPadding[3]{};
			uint32_t indirect[4]{};
			float4 falloffParameters;
			float4 stochasticParameters;
		};
		const auto& origin = globals::game::frameBufferCached.GetCameraPosAdjust();
		const Constants constants{ a_frame.size[0], a_frame.size[1], a_frame.number, uint32_t(a_frame.scene->bottoms.size()), a_direction, kRayMaximum, { origin.x, origin.y, origin.z }, kRayMinimum, a_localLight ? 0.0f : kSunAngularRadius, { a_localLight ? 1u : 0u, a_frame.materialView, a_frame.allPreparedLights ? uint32_t(a_frame.lightIdentities.size()) : 0u }, a_frame.strengths, (a_frame.useDirectRR || a_frame.indirectRays ||
            a_frame.nativeDirectRRRequested) ? 1u : 0u, { 0u, a_frame.directTraceMode, a_frame.indirectTraceMode }, { a_frame.indirectRays, a_frame.bindings->materials->roomCount, a_frame.bindings->materials->roomsKnown ? 1u : 0u, a_frame.normalMode | a_frame.rrMaterialPolicy }, a_frame.falloffParameters, { a_frame.stochasticParameters.x, a_frame.stochasticParameters.y,
				a_frame.shadowMask ? 1.0f : 0.0f, a_frame.stochasticParameters.w } };
		static_assert(sizeof(Constants) == 36 * sizeof(uint32_t));
		commands->SetComputeRoot32BitConstants(5, 36, &constants, 0);
		std::vector<D3D12_RESOURCE_BARRIER> begin{
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.light->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.receivers->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.output->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
		};
		for (const auto& signal : a_frame.directSignals) if (signal) begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(signal->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		if (a_frame.visibilityBits) begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(a_frame.visibilityBits.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		if (a_frame.shadowMask) begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(a_frame.shadowMask.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		if (a_frame.shadowMaskExport) begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(a_frame.shadowMaskExport->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		commands->ResourceBarrier(uint32_t(begin.size()), begin.data());
		if (a_frame.shadowMask && a_frame.visibilityBits) {
			Constants maskPass = constants;
			maskPass.signalPadding[0] = 4u;
			commands->SetComputeRoot32BitConstants(5, 36, &maskPass, 0);
			commands->Dispatch((a_frame.size[0] + 7) / 8, (a_frame.size[1] + 7) / 8, 1);
			++shadowMaskTraceDispatches;
			std::vector<D3D12_RESOURCE_BARRIER> maskBarriers{
				CD3DX12_RESOURCE_BARRIER::UAV(a_frame.visibilityBits.get()),
				CD3DX12_RESOURCE_BARRIER::UAV(a_frame.shadowMask.get())
			};
			if (a_frame.shadowMaskExport)
				maskBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(a_frame.shadowMaskExport->resource.get()));
			commands->ResourceBarrier(uint32_t(maskBarriers.size()), maskBarriers.data());
			if (a_frame.directTraceMode != 0u) {
				maskPass.signalPadding[0] = 5u;
				commands->SetComputeRoot32BitConstants(5, 36, &maskPass, 0);
				commands->Dispatch((a_frame.size[0] + 7) / 8, (a_frame.size[1] + 7) / 8, 1);
				++shadowMaskResolveDispatches;
				commands->ResourceBarrier(uint32_t(maskBarriers.size()), maskBarriers.data());
			}
			commands->SetComputeRoot32BitConstants(5, 36, &constants, 0);
		}
		commands->Dispatch((a_frame.size[0] + 7) / 8, (a_frame.size[1] + 7) / 8, 1);
		++directTraceDispatches;
		if (a_frame.directTraceMode != 0u && !a_frame.fsrIndependent) {
            std::vector<D3D12_RESOURCE_BARRIER> barriers{
				CD3DX12_RESOURCE_BARRIER::UAV(a_frame.output->resource.get())
			};
			for (uint32_t i = 0; i < 5; ++i)
				if (a_frame.directSignals[i]) barriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(a_frame.directSignals[i]->resource.get()));
			commands->ResourceBarrier(uint32_t(barriers.size()), barriers.data());
			Constants resolve = constants;
			resolve.signalPadding[0] = 2u;
			commands->SetComputeRoot32BitConstants(5, 36, &resolve, 0);
			commands->Dispatch((a_frame.size[0] + 7) / 8, (a_frame.size[1] + 7) / 8, 1);
		}
		if (a_frame.indirectRays != 0) {
			// Dedicated indirect dispatch; the same retained scene and light snapshot
			// are consumed, while only indirect signal UAVs are written.
			// UAV ordering: the direct pass initialized these signal resources.
			std::array<D3D12_RESOURCE_BARRIER, 2> independentSignals{
				CD3DX12_RESOURCE_BARRIER::UAV(a_frame.directSignals[5]->resource.get()),
				CD3DX12_RESOURCE_BARRIER::UAV(a_frame.directSignals[6]->resource.get())
			};
			commands->ResourceBarrier(uint32_t(independentSignals.size()), independentSignals.data());
			Constants indirectConstants = constants;
			indirectConstants.signalPadding[0] = 1u;
			commands->SetComputeRoot32BitConstants(5, 36, &indirectConstants, 0);
			commands->Dispatch((a_frame.size[0] + 7) / 8, (a_frame.size[1] + 7) / 8, 1);
			++indirectTraceDispatches;
            if (a_frame.transportTier >= 2u && a_frame.transportTier <= 5u)
                ++advancedTransportDispatches;
			// Advanced stochastic families are deliberately sparse. Their RR
            // contexts must see the original validity masks, not a spatially
            // expanded nearest-anchor signal before temporal reconstruction.
            if (a_frame.indirectTraceMode != 0u && a_frame.transportTier == 0u) {
				std::array<D3D12_RESOURCE_BARRIER, 2> resolveOrder{
					CD3DX12_RESOURCE_BARRIER::UAV(a_frame.directSignals[5]->resource.get()),
					CD3DX12_RESOURCE_BARRIER::UAV(a_frame.directSignals[6]->resource.get())
				};
				commands->ResourceBarrier(uint32_t(resolveOrder.size()), resolveOrder.data());
				indirectConstants.signalPadding[0] = 3u;
				commands->SetComputeRoot32BitConstants(5, 36, &indirectConstants, 0);
				commands->Dispatch((a_frame.size[0] + 7) / 8, (a_frame.size[1] + 7) / 8, 1);
			}
		}
        // Resolve preserves true checkerboard anchors, read those exclusively.
        if (a_frame.nativeDirectRRRequested)
            (void)RecordNativeDirectRRPack(a_frame);
		const auto ordered = CD3DX12_RESOURCE_BARRIER::UAV(a_frame.output->resource.get());
		commands->ResourceBarrier(1, &ordered);
		std::vector<D3D12_RESOURCE_BARRIER> end{
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.light->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.receivers->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.output->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)
		};
		for (const auto& signal : a_frame.directSignals) if (signal) end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(signal->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
		if (a_frame.visibilityBits) end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(a_frame.visibilityBits.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
		if (a_frame.shadowMask) end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(a_frame.shadowMask.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
		if (a_frame.shadowMaskExport) end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(a_frame.shadowMaskExport->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
		commands->ResourceBarrier(uint32_t(end.size()), end.data());
        // Shared low-tier shadow mask temporal resolve MUST run before either
        // DX11 Shadow Only export consumption or DX12 Hybrid source/GI.
        if (a_frame.fsrIndependent) {
            // Preserve original Direct signal production/packing regardless
            // of GI status. RR is optional; raw Direct remains composable.
            if (a_frame.fsrDepthReady && a_frame.nativeDirectRRRequested &&
                globals::features::rtComposer.composition.useFSRRayRegeneration)
                (void)RecordNativeDirectRR(a_frame);
            (void)RecordFSRIndependentComposition(a_frame);
        } else {
            RecordShadowTemporal(a_frame);
            if (a_frame.hybridInputsReady) {
                RecordHybridShadowedSource(a_frame);
                if (a_frame.nativeDirectRRRequested) {
                    if (RecordNativeDirectRR(a_frame) &&
                        globals::features::rtComposer.NativeDirectRRPreviewActive())
                        (void)RecordNativeDirectRRComposition(a_frame);
                }
                RecordNativeHybridGI(a_frame);
            }
        }
        // FSR Independent never invokes Open RT temporal, GI, or composition.
		DX::ThrowIfFailed(commands->Close());
		ID3D12CommandList* lists[]{ commands };
		backend.commandQueue->ExecuteCommandLists(1, lists);
		a_frame.fence = ++nextCompletion;
		DX::ThrowIfFailed(backend.commandQueue->Signal(completion.fence12.get(), a_frame.fence));
		++submissions;
	}

	void DirectionalTrace::Consume(Frame& a_frame)
	{
		CS_GPU_PASS("RTDirectLighting::PreviewRawVisibility");
		if (previewSize != a_frame.size || !preview) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_frame.size[0]; desc.Height = a_frame.size[1];
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			preview = std::make_unique<Texture2D>(desc, "RTDirectLighting::Directional Preview");
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = desc.Format;
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srv.Texture2D.MipLevels = 1;
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = desc.Format;
			uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			preview->CreateSRV(srv); preview->CreateUAV(uav);
			previewSize = a_frame.size;
			const auto allocationDesc = CD3DX12_RESOURCE_DESC::Tex2D(desc.Format, desc.Width, desc.Height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			previewBytes = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &allocationDesc).SizeInBytes;
		}
		auto* context = backend.d3d11Context.get();
		DX::ThrowIfFailed(context->Wait(completion.fence11.get(), a_frame.fence));
		winrt::com_ptr<ID3D11ComputeShader> shader;
		winrt::com_ptr<ID3D11ShaderResourceView> input;
		winrt::com_ptr<ID3D11UnorderedAccessView> output;
		context->CSGetShader(shader.put(), nullptr, nullptr);
		context->CSGetShaderResources(0, 1, input.put());
		context->CSGetUnorderedAccessViews(0, 1, output.put());
		auto* target = preview->uav.get();
		context->CSSetShaderResources(0, 1, &a_frame.output->srv);
		context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
		context->CSSetShader(display.get(), nullptr, 0);
		context->Dispatch((a_frame.size[0] + 7) / 8, (a_frame.size[1] + 7) / 8, 1);
		auto* priorInput = input.get(); target = output.get();
		context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
		context->CSSetShaderResources(0, 1, &priorInput);
		context->CSSetShader(shader.get(), nullptr, 0);
		a_frame.retirement = producer.Next();
		DX::ThrowIfFailed(context->Signal(producer.fence11.get(), a_frame.retirement));
		previewFrame = a_frame.number;
		previewMaterialView = a_frame.materialView;
		previewValid = true;
	}

	void DirectionalTrace::Retire()
	{
		if (!completion.fence12) return;
		const auto done = completion.fence12->GetCompletedValue();
		const auto retired = producer.fence12->GetCompletedValue();
		if (done == UINT64_MAX || retired == UINT64_MAX) { failure = "Directional device removed"; previewValid = false; return; }
		try {
			for (auto& frame : frames) {
				if (!frame.submitted || !frame.fence || frame.fence > done) continue;
				if (!frame.retirement && !frame.fsrHybridOnly && frame.generation == generation && display) { Consume(frame); ++completions; }
				if (frame.retirement && frame.retirement > retired) continue;
				if (frame.generation != generation) { frame = {}; continue; }
				frame.scene.reset();
				frame.bindings.reset();
				frame.submitted = false;
				frame.fence = frame.retirement = 0;
			}
		} catch (const std::exception& error) { failure = error.what(); previewValid = false; }
	}

	void DirectionalTrace::CompositeScene(Frame& a_frame, ID3D11UnorderedAccessView* a_target)
	{
		CS_GPU_PASS("RTDirectLighting::DirectSceneComposite");
		auto* context = backend.d3d11Context.get();
		struct Settings {
            uint32_t indirect, directRR; float indirectStrength; uint32_t useDXRIndirect;
            float redstoneBlend; uint32_t shadowOnly; float shadowInfluence;
            float shadowEdgeFilterEnabled; float contactAAWeight, contactAADepthTolerance;
        };
		if (!lightingCompositeSettings) lightingCompositeSettings = std::make_unique<ConstantBuffer>(ConstantBufferDesc<Settings>(), "RTDirectLighting::LightingComposite CB");
		winrt::com_ptr<ID3D11ComputeShader> shader;
		std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 17> inputs;
		std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 6> indirectInputs;
		std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, 7> outputs;
		winrt::com_ptr<ID3D11Buffer> shared, features, settings, camera;
		context->CSGetShader(shader.put(), nullptr, nullptr);
		for (uint32_t i = 0; i < inputs.size(); ++i) {
			context->CSGetShaderResources(i, 1, inputs[i].put());
			if (i < indirectInputs.size()) context->CSGetShaderResources(20 + i, 1, indirectInputs[i].put());
		}
		for (uint32_t i = 0; i < outputs.size(); ++i) context->CSGetUnorderedAccessViews(i, 1, outputs[i].put());
		context->CSGetConstantBuffers(0, 1, settings.put());
		context->CSGetConstantBuffers(5, 1, shared.put());
		context->CSGetConstantBuffers(6, 1, features.put());
		context->CSGetConstantBuffers(12, 1, camera.put());
		DX::ThrowIfFailed(context->Wait(completion.fence11.get(), a_frame.fence));
        const auto& composer = globals::features::rtComposer;
        const bool nativeHybridActive = composer.DX12HybridActive();
        // A complete DX12 Direct RR preview can replace ONLY the final
        // scene image; its raw signals and SDK contexts never go via DX11.
        const bool fsrReady = composer.FSRIndependentActive() &&
            a_frame.fsrIndependent && a_frame.fsrIndependentComposed &&
            a_frame.fsrIndependentScene;
        const bool nativeDirectPreview = composer.NativeDirectRRPreviewActive() &&
            a_frame.nativeDirectRRComposed && a_frame.nativeDirectRRReady &&
            a_frame.nativeDirectRRSuccessMask == 3u &&
            a_frame.nativeDirectRRScene;
        const auto* finalNative = composer.AdvancedActive() &&
            composer.composition.mode >= RTComposer::CompositorMode::RTEnhanced ?
            &a_frame.postHybridGI : &a_frame.nativeGI;
        if (fsrReady || nativeDirectPreview ||
            (nativeHybridActive && finalNative->composedValid &&
             finalNative->composedScene)) {
            if (!nativeHybridResolve && nativeHybridPreviewFailure.empty()) {
                try {
                    nativeHybridResolve.attach(static_cast<ID3D11ComputeShader*>(
                        Util::CompileShader(L"Data\\Shaders\\RTDirectLighting\\nativeHybridResolve.cs.hlsl",
                            {}, "cs_5_0")));
                    if (!nativeHybridResolve)
                        nativeHybridPreviewFailure = "Native hybrid DX11 output shader missing";
                } catch (const std::exception& error) {
                    nativeHybridPreviewFailure = error.what();
                }
            }
            if (nativeHybridResolve) {
                auto* nativeOutput = fsrReady ? a_frame.fsrIndependentScene->srv :
                    nativeDirectPreview ? a_frame.nativeDirectRRScene->srv :
                    finalNative->composedScene->srv;
                context->CSSetShaderResources(0, 1, &nativeOutput);
                context->CSSetUnorderedAccessViews(0, 1, &a_target, nullptr);
                context->CSSetShader(nativeHybridResolve.get(), nullptr, 0);
                context->Dispatch((a_frame.size[0] + 7u) / 8u,
                    (a_frame.size[1] + 7u) / 8u, 1);
                auto* oldOutput = outputs[0].get();
                auto* oldInput = inputs[0].get();
                context->CSSetUnorderedAccessViews(0, 1, &oldOutput, nullptr);
                context->CSSetShaderResources(0, 1, &oldInput);
                context->CSSetShader(shader.get(), nullptr, 0);
                a_frame.retirement = producer.Next();
                DX::ThrowIfFailed(context->Signal(producer.fence11.get(), a_frame.retirement));
                ++sceneCompositeSubmissions;
                ++nativeHybridPreviewSubmissions;
                if (nativeDirectPreview) ++nativeDirectRRPresentedFrames;
                if (fsrReady) ++fsrIndependentPresentedFrames;
                if (composer.AdvancedActive()) ++advancedPresentedFrames;
                sceneCompositeFrame = a_frame.number;
                return;
            }
        }
        if (nativeHybridActive || composer.IsFSRRenderer()) {
            // FSR failure is raster-only. Never execute Open RT's DX11
            // Direct composer, denoiser, or Shadow Only fallback.
            ++nativeHybridFallbackFrames;
            if (composer.AdvancedActive()) ++advancedRasterFallbackFrames;
            a_frame.retirement = producer.Next();
            DX::ThrowIfFailed(context->Signal(producer.fence11.get(), a_frame.retirement));
            return;
        }
		globals::state->BindSharedDataCS(context);
		auto* cameraBuffer = a_frame.camera.get();
		context->CSSetConstantBuffers(12, 1, &cameraBuffer);
		const uint32_t rrFlags = DenoiseDirect(a_frame);
		PrepareRRPreview(a_frame, rrFlags);
		const bool directRR = (rrFlags & 1u) != 0;
        // Advanced preset transport is diagnostic until native sparse RR and
        // composition exist; do not leak its raw radiance to the DX11 compositor.
        const bool composeDXRIndirect = a_frame.indirectRays != 0u &&
            a_frame.transportTier == 0u;
		auto& indirect = globals::features::redstoneGI;
		const bool useIndirect = a_frame.useIndirect && indirect.HasCurrentIndirectLighting();
		if (useIndirect) indirect.BindCompositeResources();
		const bool shadowOnly = composer.ShadowOnlyActive() && a_frame.shadowMaskExport;
        const auto shadowTier = RTComposer::GetTierPolicy(RTComposer::CompositorMode::ShadowOnly);
        const auto& dev = composer.EffectiveDeveloperControls();
        const float directBlend = composer.DeveloperSceneOverride() ?
            dev.sceneDirectBlend : (composer.IsFixedPreset() ?
                shadowTier.directContribution : composer.composition.rtDirectContribution);
        const float shadowStrength = composer.DeveloperSceneOverride() ?
            dev.sceneShadowInfluence : (composer.IsFixedPreset() ?
                shadowTier.shadowInfluence : composer.composition.shadowInfluence);
		lightingCompositeSettings->Update(Settings{ useIndirect ? 1u : 0u, rrFlags, a_frame.indirectStrength,
			composeDXRIndirect ? 1u : 0u,
			a_frame.rasterBlend * (shadowOnly ? directBlend : 1.0f),
			shadowOnly ? 1u : 0u, shadowOnly ? shadowStrength : 0.0f,
            shadowOnly && composer.IsFixedPreset() &&
                (!composer.advancedDeveloperMode || dev.shadowContactAAEnabled) ? 1.0f : 0.0f,
            dev.shadowContactAAWeight, dev.shadowContactAADepthTolerance });
		auto* cb = lightingCompositeSettings->CB();
		context->CSSetConstantBuffers(0, 1, &cb);
		const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
		ID3D11ShaderResourceView* reads[]{ a_frame.output->srv, Util::AsReal(targets[NORMALROUGHNESS].SRV),
			directRR ? a_frame.directSignals[0]->srv : nullptr, directRR ? a_frame.directSignals[1]->srv : nullptr,
			directRR ? directRRTextures[2]->srv.get() : nullptr, directRR ? directRRTextures[3]->srv.get() : nullptr, directRR ? a_frame.directSignals[3]->srv : nullptr,
			composeDXRIndirect ? a_frame.directSignals[5]->srv : nullptr, composeDXRIndirect ? a_frame.directSignals[6]->srv : nullptr,
			(rrFlags & 2u) ? directRRTextures[4]->srv.get() : nullptr, (rrFlags & 2u) ? directRRTextures[5]->srv.get() : nullptr,
			rrFlags ? directRRTextures[0]->srv.get() : nullptr, rrFlags ? directRRTextures[10]->srv.get() : nullptr, a_frame.receivers->srv,
			rrFlags ? a_frame.directSignals[2]->srv : nullptr,
			directRR ? a_frame.directSignals[4]->srv : nullptr,
			shadowOnly ? a_frame.shadowMaskExport->srv : nullptr };
		context->CSSetShaderResources(0, 17, reads);
		context->CSSetUnorderedAccessViews(0, 1, &a_target, nullptr);
		context->CSSetShader(composite.get(), nullptr, 0);
		context->Dispatch((a_frame.size[0] + 7) / 8, (a_frame.size[1] + 7) / 8, 1);
		if (useIndirect) { indirect.FinishComposite(true); ++indirectCompositeSubmissions; }
		for (uint32_t i = 0; i < outputs.size(); ++i) {
			auto* output = outputs[i].get();
			context->CSSetUnorderedAccessViews(i, 1, &output, nullptr);
		}
		for (uint32_t i = 0; i < inputs.size(); ++i) {
			auto* input = inputs[i].get();
			auto* indirectInput = i < indirectInputs.size() ? indirectInputs[i].get() : nullptr;
			context->CSSetShaderResources(i, 1, &input);
			if (i < indirectInputs.size()) context->CSSetShaderResources(20 + i, 1, &indirectInput);
		}
		ID3D11Buffer* priorShared[]{ shared.get(), features.get() };
		cb = settings.get(); cameraBuffer = camera.get();
		context->CSSetConstantBuffers(0, 1, &cb);
		context->CSSetConstantBuffers(5, 2, priorShared);
		context->CSSetConstantBuffers(12, 1, &cameraBuffer);
		context->CSSetShader(shader.get(), nullptr, 0);
		a_frame.retirement = producer.Next();
		DX::ThrowIfFailed(context->Signal(producer.fence11.get(), a_frame.retirement));
		++sceneCompositeSubmissions;
		sceneCompositeFrame = a_frame.number;
	}



    void DirectionalTrace::DispatchFSRIndependentRaster(
        ID3D11UnorderedAccessView* a_sceneTarget,
        const std::array<uint32_t, 2>& a_size, uint32_t a_frame)
    {
        if (!globals::features::rtComposer.FSRIndependentActive() ||
            !a_sceneTarget || !a_size[0] || !a_size[1] ||
            a_size[0] > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            a_size[1] > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            !completion.fence12 || !producer.fence12 ||
            (sceneCompositeSubmissions && sceneCompositeFrame == a_frame)) return;

        // Existing tracer slots, backend queue and producer/completion fences
        // remain the sole owners. No scene, TLAS, materials or Direct buffers.
        Frame* available = nullptr;
        for (auto& frame : frames) {
            if (frame.scene || frame.submitted) continue;
            if (!available || frame.number < available->number)
                available = &frame;
        }
        if (!available) return;
        try {
            if (!available->fsrHybridOnly || available->size != a_size)
                *available = {};
            available->size = a_size;
            available->number = a_frame;
            available->generation = generation;
            available->fsrHybridOnly = true;
            available->fsrIndependent = true;
            available->fsrIndependentComposed = false;
            available->fsrDepthReady = false;
            available->nativeGIRequested = true;
            available->nativeGI.rrValid = false;
            available->nativeGI.valid = false;
            available->nativeDirectRRRequested = false;
            available->nativeDirectRRReady = false;
            available->nativeDirectRRSuccessMask = 0u;
            available->fence = available->retirement = 0u;
            available->submitted = false;

            const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const auto linearDesc = CD3DX12_RESOURCE_DESC::Tex2D(
                DXGI_FORMAT_R32_FLOAT, a_size[0], a_size[1], 1, 1, 1, 0,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if (!available->hybridLinearDepth)
                DX::ThrowIfFailed(backend.d3d12Device->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &linearDesc,
                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                    IID_PPV_ARGS(available->hybridLinearDepth.put())));

            static constexpr std::array names{
                "RTComposer::HybridScene", "RTComposer::HybridDepth",
                "RTComposer::HybridNormalRoughness", "RTComposer::HybridAlbedo",
                "RTComposer::HybridReflectance", "RTComposer::HybridMotion" };
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = a_size[0];
            desc.Height = a_size[1];
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            for (uint32_t i = 0; i < kHybridInputCount; ++i) {
                if (available->hybridInputs[i]) continue;
                desc.Format = i == 1u ?
                    DXGI_FORMAT_R32G32B32A32_FLOAT :
                    DXGI_FORMAT_R16G16B16A16_FLOAT;
                available->hybridInputs[i] = std::make_unique<WrappedResource>(
                    desc, backend.d3d11Device.get(), backend.d3d12Device.get(), names[i]);
            }
            PrepareHybridInputs(*available);
            if (!available->hybridInputsReady) return;

            const auto produced = producer.Next();
            DX::ThrowIfFailed(backend.d3d11Context->Signal(
                producer.fence11.get(), produced));
            backend.d3d11Context->Flush();
            DX::ThrowIfFailed(backend.commandQueue->Wait(
                producer.fence12.get(), produced));
            available->submitted = true;
            Record(*available, {}, false);
            CompositeScene(*available, a_sceneTarget);
        } catch (const std::exception& error) {
            fsrIndependentGIFailure = error.what();
            if (available->submitted && !available->fence) {
                // Retain resources until the enqueued producer fence has
                // retired even if subsequent command recording failed.
                const uint64_t fenceValue = ++nextCompletion;
                if (SUCCEEDED(backend.commandQueue->Signal(
                    completion.fence12.get(), fenceValue)))
                    available->fence = fenceValue;
            }
            if (!available->submitted) *available = {};
        }
    }

	void DirectionalTrace::Dispatch(std::shared_ptr<const TopLevel> a_scene, const std::array<uint32_t, 2>& a_nativeSize, const float3& a_lightDirection, uint32_t a_frame, uint64_t a_budgetBytes, bool a_localLight, const LightLimitFixData::LightView& a_lights, uint32_t a_materialView, ID3D11UnorderedAccessView* a_sceneTarget, uint32_t a_lightReference, uint32_t a_lightBase, ID3D11ShaderResourceView* a_roomMetadata, std::span<const std::array<uint32_t, 2>> a_lightIdentities, bool a_allPreparedLights, float4 a_strengths, bool a_useIndirect, float a_indirectStrength, bool a_useDirectRR, uint32_t a_indirectRays, std::span<const uint32_t> a_roomReferences, float a_rasterBlend, uint32_t a_normalMode, std::span<const PreparedPointLight> a_preparedLights, uint32_t a_directResolutionMode, uint32_t a_indirectResolutionMode, uint32_t a_directRRResolutionMode, bool a_rrMaterialIsolation, float a_rrGlossyRoughness, float4 a_falloffParameters, float4 a_stochasticParameters)
	{
		CS_GPU_PASS("RTDirectLighting::DirectionalDispatch");
		const bool direct = a_materialView >= kFirstDirectView;
		lightSelectionFailure.clear();
		if (a_allPreparedLights && (!direct || a_preparedLights.empty() || a_preparedLights.size() > kMaximumDirectLights)) {
			lightSelectionFailure = "All-prepared-light mode requires 1..32 prepared lights and a direct-light view";
			previewValid = false;
			return;
		}
		if (a_sceneTarget && (a_materialView != kCombinedDirectView || (sceneCompositeSubmissions && sceneCompositeFrame == a_frame))) return;
		const bool validLights = a_lights.frame == a_frame && a_lights.roomGeneration != 0;
		const auto lights = validLights ? a_lights : LightLimitFixData::LightView{};
		if (a_localLight && (!a_materialView || direct) && !validLights) ++unavailableLightFrames;
		if (!pipeline || !failure.empty() || !a_scene || a_scene->instances.empty() || a_scene->instances.size() > kMaximumInstances || !a_nativeSize[0] || !a_nativeSize[1]) return;
		if (a_nativeSize[0] > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || a_nativeSize[1] > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
			!std::isfinite(a_lightDirection.x) || !std::isfinite(a_lightDirection.y) || !std::isfinite(a_lightDirection.z)) return;
		Frame* available = nullptr;
        // Alternate retired slots, but never overwrite a history producer
        // while the other frame's queued DX12 pass still reads its textures.
        for (auto& frame : frames) {
            if (frame.scene || frame.submitted) continue;
            const auto index = int32_t(&frame - frames.data());
            const bool referenced = std::ranges::any_of(frames, [&](const Frame& other) {
                return &other != &frame && other.submitted &&
                    other.shadowHistorySourceSlot == index;
            });
            if (referenced) continue;
            if (!available || frame.number < available->number)
                available = &frame;
        }
		if (!available) { ++busyFrames; return; }
		if ((!cachedBindings || cachedBindings->scene != a_scene) && std::ranges::any_of(frames, [&](const Frame& frame) {
			return frame.bindings && frame.scene != a_scene && frame.bindings->materials && frame.bindings->materials->bytes;
		})) { ++bindingDeferrals; return; }
		if (pendingBindings) {
			if (pendingBindings->busy.load(std::memory_order_acquire)) { ++bindingDeferrals; return; }
			if (pendingBindings->scene == a_scene) {
				if (!pendingBindings->failure.empty()) { failure = pendingBindings->failure; previewValid = false; return; }
				if (pendingBindings->value && pendingBindings->value->scene == a_scene) { cachedBindings = pendingBindings->value; ++bindingCreations; }
			}
			pendingBindings.reset();
		}
		if (cachedBindings && cachedBindings->scene != a_scene) cachedBindings.reset();
		const auto bindingBytes = cachedBindings ? 0 : BindingBytes(backend, *a_scene);
		if (bindingBytes == UINT64_MAX) { failure = "Directional binding allocation unsupported"; return; }
		const std::array<uint32_t, 2> size = a_sceneTarget ? a_nativeSize : std::array<uint32_t, 2>{ (a_nativeSize[0] + kGridDivisor - 1) / kGridDivisor, (a_nativeSize[1] + kGridDivisor - 1) / kGridDivisor };
		const auto& composer = globals::features::rtComposer;
        const bool fsrIndependent = a_sceneTarget && composer.FSRIndependentActive();
        const bool wantEarlyMask = a_sceneTarget &&
            (!fsrIndependent || composer.composition.fsrHybrid.earlyShadowMask);
        bool nativeDirectTest = a_sceneTarget && composer.NativeDirectRRDiagnosticActive() &&
            size[0] >= 2u && (size[0] & 1u) == 0u && size[1] != 0u;
        // Disabled direct RR does not retain dedicated AMD contexts indefinitely.
        // The provider's own queue fence prevents retiring in-flight dispatches.
        if (!nativeDirectTest && nativeDirectRRDenoiser &&
            nativeDirectRRDenoiser->RetireNativeHybridContexts())
            nativeDirectRRDenoiser.reset();
        const auto rrPolicy = RayRegeneration::GetSettings();
		const uint32_t effectiveRRMode = a_directRRResolutionMode == 3u ? rrPolicy.resolutionMode : std::min(a_directRRResolutionMode, 2u);
		const bool directRR = a_sceneTarget && a_useDirectRR && rrPolicy.enabled;
		const bool wantExport = a_sceneTarget && globals::features::rtComposer.ShadowOnlyActive();
        const auto& shadowComposer = globals::features::rtComposer;
        const auto& shadowDev = shadowComposer.EffectiveDeveloperControls();
        bool wantShadowTemporal = !fsrIndependent && a_sceneTarget &&
            shadowComposer.TierControlsLocked() &&
            (shadowComposer.composition.mode == RTComposer::CompositorMode::ShadowOnly ||
             shadowComposer.composition.mode == RTComposer::CompositorMode::HybridShadowed) &&
            (!shadowComposer.advancedDeveloperMode ||
             !shadowDev.overrideShadowTemporal || shadowDev.shadowTemporalEnabled);
        // Acquire DX11 inputs only when production native Hybrid-Shadowed is
        // eligible; Shadow Only is the sole DX11 scene-composition mode.
        bool wantHybridInputs = a_sceneTarget &&
            (composer.DX12HybridActive() || nativeDirectTest);
        bool wantNativeGI = a_sceneTarget &&
            (fsrIndependent || (composer.DX12HybridActive() &&
                NativeHybridGI::ShadersAvailable()));
        const auto& composition = composer.composition;
        // Compute the intended second-pass resource requirement before
        // frame-reuse checks; later VRAM fallback can disable it.
        bool wantPostHybrid = wantNativeGI && composer.AdvancedActive() &&
            composition.mode >= RTComposer::CompositorMode::RTEnhanced;
        const auto fixed = RTComposer::GetTierPolicy(
            composer.AdvancedActive() ? composer.composition.mode :
                RTComposer::CompositorMode::HybridShadowed);
        const auto hybridSampling = MakeHybridSamplingLayout(
            (fsrIndependent || composer.LegacyHybridPrototypeActive()) ?
                composition.fsrHybrid.resolution :
                (composer.AdvancedActive() ?
                    (composer.composition.mode <= RTComposer::CompositorMode::RTExclusiveII ? 1u : 0u) :
                    (composer.IsFixedPreset() ? fixed.hybridResolution : composition.hybridResolution)),
            (fsrIndependent || composer.LegacyHybridPrototypeActive()) ?
                composition.fsrHybrid.checkerboard :
                (composer.AdvancedActive() ?
                    (composer.composition.mode >= RTComposer::CompositorMode::RTEnhanced) :
                    (composer.IsFixedPreset() ? fixed.hybridCheckerboard : composition.hybridCheckerboard)),
            size, a_frame);
        const auto effectiveRecon = composer.EffectiveReconstructionPolicy().reconstruction;
        const std::array<bool, 3> optionalBlur{
            effectiveRecon[0].blurEnabled, effectiveRecon[1].blurEnabled,
            effectiveRecon[2].blurEnabled };
        const uint32_t requestedBlurMask = uint32_t(optionalBlur[0]) |
            (uint32_t(optionalBlur[1]) << 1u) |
            (uint32_t(optionalBlur[2]) << 2u);
		bool reuse = available->size == size &&
            available->reconstructionBlurMask == requestedBlurMask &&
            available->useDirectRR == directRR &&
            available->nativeDirectRRRequested == nativeDirectTest && available->indirectRays == a_indirectRays && available->receivers && available->output && available->light &&
			bool(available->shadowMask) == wantEarlyMask &&
			bool(available->shadowMaskExport) == wantExport &&
            bool(available->hybridInputs[0]) == wantHybridInputs &&
            bool(available->shadowTemporal) == wantShadowTemporal &&
            bool(available->shadowMotion) == wantShadowTemporal &&
            (!wantHybridInputs || (available->hybridShadowedSource && available->hybridLinearDepth)) &&
            bool(available->nativeGI.textures[0]) == wantNativeGI &&
            bool(available->postHybridGI.textures[0]) == wantPostHybrid &&
            (!wantNativeGI || available->nativeGI.constants) &&
            (!wantEarlyMask || available->visibilityBits);
		constexpr uint64_t kFrameOverheadBytes = 1024 * 1024;
		const auto textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, size[0], size[1], 1, 1, 1, 0,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
		const auto previewDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, size[0], size[1], 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		const auto textureBytes = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &textureDesc).SizeInBytes;
		const auto lightDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, kLightSnapshotWidth, 2, 1, 1, 1, 0,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
		const auto lightBytes = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &lightDesc).SizeInBytes;
		const auto nextPreviewBytes = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &previewDesc).SizeInBytes;
		// Visibility and shadow mask are produced/consumed on the same DX12 queue.
		// Do not request D3D11 sharing or simultaneous-access flags for these UAVs.
		const auto visibilityDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32_UINT, size[0], size[1], 1, 1, 1, 0,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		const auto maskDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16_FLOAT, size[0], size[1], 1, 1, 1, 0,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		const uint64_t visibilityBytes = wantEarlyMask ? backend.d3d12Device->GetResourceAllocationInfo(0, 1, &visibilityDesc).SizeInBytes : 0;
		const uint64_t maskBytes = wantEarlyMask ? backend.d3d12Device->GetResourceAllocationInfo(0, 1, &maskDesc).SizeInBytes : 0;
		const uint64_t exportBytes = wantExport ? nextPreviewBytes : 0;
        uint64_t temporalBytes = wantShadowTemporal ? maskBytes + nextPreviewBytes : 0;
        const auto linearDepthDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, size[0], size[1], 1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const uint64_t linearDepthBytes = wantHybridInputs ?
            backend.d3d12Device->GetResourceAllocationInfo(0, 1, &linearDepthDesc).SizeInBytes : 0;
        uint64_t hybridBytes = wantHybridInputs ? nextPreviewBytes * kHybridInputCount + textureBytes + linearDepthBytes : 0;
        // Conservative DX12 reserve for half-width raw checkerboard, five
        // full-native material/motion guides and two AMD output textures.
        uint64_t nativeDirectBytes = nativeDirectTest ?
            nextPreviewBytes * 12u + kFrameOverheadBytes : 0u;
        // Optional blur is already resolved for frame-reuse and budget checks.
        uint64_t nativeGIBytes = wantNativeGI ?
            NativeHybridGI::EstimateBytes(backend.d3d12Device.get(), size[0], size[1],
                hybridSampling, optionalBlur) : 0;
        // Historical allocations from earlier logical grids are still GPU
        // resident until the NativeHybridGI owner retires. Include them in
        // the conservative budget instead of counting only active textures.
        if (wantNativeGI && nativeGIBytes != UINT64_MAX && nativeHybridGI) {
            const uint64_t retained = nativeHybridGI->RetainedNativeHistoryBytes();
            nativeGIBytes = retained > UINT64_MAX - nativeGIBytes ?
                UINT64_MAX : nativeGIBytes + retained;
        }
        if (wantPostHybrid && nativeGIBytes != UINT64_MAX) {
            const auto postSampling = MakeHybridSamplingLayout(2u, true, size, a_frame);
            uint64_t additional = postSampling.checkerboard ?
                NativeHybridGI::EstimateBytes(backend.d3d12Device.get(), size[0],
                    size[1], postSampling, optionalBlur) : UINT64_MAX;
            if (additional != UINT64_MAX && postHybridGI) {
                const uint64_t retained = postHybridGI->RetainedNativeHistoryBytes();
                additional = retained > UINT64_MAX - additional ?
                    UINT64_MAX : additional + retained;
            }
            nativeGIBytes = additional == UINT64_MAX ||
                nativeGIBytes > UINT64_MAX - additional ?
                UINT64_MAX : nativeGIBytes + additional;
        }
        if (nativeGIBytes == UINT64_MAX) {
            // Unsupported optional native dispatch must not block shadows.
            wantNativeGI = false;
            wantPostHybrid = false;
            wantHybridInputs = false;
            hybridBytes = nativeGIBytes = 0;
        }
		if (textureBytes == UINT64_MAX || lightBytes == UINT64_MAX || nextPreviewBytes == UINT64_MAX ||
			visibilityBytes == UINT64_MAX || maskBytes == UINT64_MAX) { failure = "Directional texture allocation unsupported"; return; }
		uint64_t signalBytes = (directRR || a_indirectRays || nativeDirectTest) ? nextPreviewBytes * kDirectSignalCount : 0;
		const uint64_t rrInteropReserve = directRR ? uint64_t(std::max(1u, size[0] >> effectiveRRMode)) * std::max(1u, size[1] >> effectiveRRMode) * 256 + kFrameOverheadBytes : 0;
		auto calculateRequired = [&] {
            return (reuse ? uint64_t{ 0 } : textureBytes * 2 + lightBytes + signalBytes + visibilityBytes + maskBytes + exportBytes + temporalBytes + hybridBytes + nativeGIBytes + nativeDirectBytes + kFrameOverheadBytes) +
                (a_sceneTarget ? uint64_t{ 0 } : nextPreviewBytes) + bindingBytes + rrInteropReserve;
        };
        uint64_t required = calculateRequired();
        uint64_t retainedBytes = ResidentBytes() - (reuse ? 0 : available->bytes);
        auto overBudget = [&] { return required > a_budgetBytes || retainedBytes > a_budgetBytes - required; };
        if (wantHybridInputs && overBudget()) {
            // The diagnostic bridge must NEVER displace functional RT shadows.
            ++hybridInputBudgetDeferrals;
            hybridInputFailure = "Hybrid capture exceeds native lighting budget; skipped";
            wantHybridInputs = false;
            wantNativeGI = false;
            wantPostHybrid = false;
            nativeDirectTest = false;
            nativeDirectBytes = 0u;
            signalBytes = (directRR || a_indirectRays) ?
                nextPreviewBytes * kDirectSignalCount : 0u;
            hybridBytes = nativeGIBytes = 0;
            reuse = available->size == size &&
            available->reconstructionBlurMask == requestedBlurMask &&
            available->useDirectRR == directRR &&
            available->nativeDirectRRRequested == nativeDirectTest &&
                available->indirectRays == a_indirectRays && available->receivers &&
                available->output && available->light &&
                bool(available->shadowMask) == wantEarlyMask &&
                bool(available->shadowMaskExport) == wantExport &&
                !available->hybridInputs[0] && !available->hybridShadowedSource &&
                !available->hybridLinearDepth && !available->nativeGI.textures[0] &&
                (!wantEarlyMask || available->visibilityBits);
            required = calculateRequired();
            retainedBytes = ResidentBytes() - (reuse ? 0 : available->bytes);
        }
        if (wantShadowTemporal && overBudget()) {
            // Denoising is optional: preserve working shadows under VRAM pressure.
            wantShadowTemporal = false;
            temporalBytes = 0;
            reuse = available->size == size &&
            available->reconstructionBlurMask == requestedBlurMask &&
            available->useDirectRR == directRR &&
            available->nativeDirectRRRequested == nativeDirectTest &&
                available->indirectRays == a_indirectRays && available->receivers &&
                available->output && available->light &&
                bool(available->shadowMask) == wantEarlyMask &&
                bool(available->shadowMaskExport) == wantExport &&
                !available->shadowTemporal && !available->shadowMotion &&
                bool(available->hybridInputs[0]) == wantHybridInputs &&
                (!wantHybridInputs || (available->hybridShadowedSource && available->hybridLinearDepth)) &&
                bool(available->nativeGI.textures[0]) == wantNativeGI &&
                bool(available->postHybridGI.textures[0]) == wantPostHybrid &&
                (!wantNativeGI || available->nativeGI.constants) &&
                (!wantEarlyMask || available->visibilityBits);
            required = calculateRequired();
            retainedBytes = ResidentBytes() - (reuse ? 0 : available->bytes);
        }
		if (overBudget()) {
            // A diagnostic must not consume the last VRAM needed by the
            // functioning Shadow Only tier. Retry with its original trace
            // schedule and no diagnostic transport, preserving raster output.
            if (a_indirectRays != 0u && a_falloffParameters.w >= 2.0f &&
                a_falloffParameters.w <= 5.0f) {
                ++budgetDeferrals;
                auto normalFalloff = a_falloffParameters;
                normalFalloff.w = 0.0f;
                Dispatch(a_scene, a_nativeSize, a_lightDirection, a_frame, a_budgetBytes,
                    a_localLight, a_lights, a_materialView, a_sceneTarget,
                    a_lightReference, a_lightBase, a_roomMetadata, a_lightIdentities,
                    a_allPreparedLights, a_strengths, a_useIndirect, a_indirectStrength,
                    a_useDirectRR, 0u, a_roomReferences, a_rasterBlend,
                    a_normalMode, a_preparedLights, a_directResolutionMode,
                    a_directResolutionMode, a_directRRResolutionMode,
                    a_rrMaterialIsolation, a_rrGlossyRoughness, normalFalloff,
                    a_stochasticParameters);
                return;
            }
			if (directRR) {
				++directRRBudgetDeferrals;
				Dispatch(a_scene, a_nativeSize, a_lightDirection, a_frame, a_budgetBytes, a_localLight, a_lights, a_materialView, a_sceneTarget,
					a_lightReference, a_lightBase, a_roomMetadata, a_lightIdentities, a_allPreparedLights, a_strengths, a_useIndirect, a_indirectStrength, false, a_indirectRays, a_roomReferences, a_rasterBlend, a_normalMode, a_preparedLights, a_directResolutionMode, a_indirectResolutionMode, a_directRRResolutionMode, a_rrMaterialIsolation, a_rrGlossyRoughness, a_falloffParameters, a_stochasticParameters);
			} else ++budgetDeferrals;
			return;
		}
		try {
			if (!reuse) *available = {};
			if (a_sceneTarget && !composite) {
				composite.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\RTDirectLighting\\compositeDirect.cs.hlsl", {}, "cs_5_0")));
				if (!composite) throw std::runtime_error("Direct-light scene compositor unavailable");
			}
			if (!cachedBindings) {
				pendingBindings = std::make_shared<PendingBindings>();
				pendingBindings->generation = generation;
				pendingBindings->scene = a_scene;
				pendingBindings->bytes = bindingBytes;
				static BS::thread_pool<> pool{ 1 };
				try {
					pool.detach_task([owned = pendingBindings, deviceBackend = backend, scene = std::move(a_scene)] {
						try { owned->value = CreateBindings(deviceBackend, scene); }
						catch (const std::exception& error) { owned->failure = error.what(); }
						owned->busy.store(false, std::memory_order_release);
					});
				} catch (...) { pendingBindings.reset(); throw; }
				++bindingDeferrals;
				return;
			}
			available->bindings = cachedBindings;
			available->descriptorBase = uint32_t(available - frames.data()) * kFrameDescriptorCount;
			available->scene = std::move(a_scene);
			available->size = size;
			available->number = a_frame;
            available->shadowTemporalEnabled = wantShadowTemporal;
            available->shadowTemporalValid = false;
            available->shadowMotionCaptured = false;
            available->shadowHistorySourceSlot = -1;
            available->nativeGIRequested = wantNativeGI;
            available->fsrIndependent = fsrIndependent;
            available->nativeDirectRRRequested = nativeDirectTest;
            available->nativeDirectPackValid = false;
            available->nativeDirectRRReady = false;
            available->nativeDirectRRComposed = false;
            available->fsrIndependentComposed = false;
            available->nativeDirectRRSuccessMask = 0u;
            available->reconstructionBlurMask = requestedBlurMask;
            available->nativeGI.valid = false;
            available->nativeGI.rrValid = false;
            available->nativeGI.composedValid = false;
			available->materialView = std::min(a_materialView, kCombinedDirectView);
			available->generation = generation;
			available->lightReference = a_lightReference;
			available->lightBase = a_lightBase;
			available->allPreparedLights = a_allPreparedLights;
			available->strengths = a_strengths;
			available->falloffParameters = {
				std::isfinite(a_falloffParameters.x) ? std::clamp(a_falloffParameters.x, 0.1f, 64.0f) : 1.0f,
				std::isfinite(a_falloffParameters.y) ? std::clamp(a_falloffParameters.y, 0.02f, 1.0f) : 0.1f,
				std::isfinite(a_falloffParameters.z) ? std::clamp(a_falloffParameters.z, 0.25f, 4.0f) : 1.0f,
                // Reserved in DXR light attenuation; used solely as a
                // staged indirect-transport tier ID. No RootSig ABI growth.
                std::isfinite(a_falloffParameters.w) ? std::clamp(a_falloffParameters.w, 0.0f, 5.0f) : 0.0f };
			available->stochasticParameters = { a_stochasticParameters.x >= 0.5f ? 1.0f : 0.0f,
				std::isfinite(a_stochasticParameters.y) ? std::clamp(a_stochasticParameters.y, 0.0f, 8.0f) : 1.0f, 0.0f, 0.0f };
			available->useIndirect = a_useIndirect && a_indirectRays == 0;
			available->indirectRays = std::min(a_indirectRays, 8u);
            available->transportTier = available->indirectRays != 0u &&
                available->falloffParameters.w >= 2.0f &&
                available->falloffParameters.w <= 5.0f ?
                uint32_t(available->falloffParameters.w) : 0u;
			available->roomReferences.fill(0);
			std::copy_n(a_roomReferences.begin(), std::min(a_roomReferences.size(), available->roomReferences.size()), available->roomReferences.begin());
			available->indirectStrength = a_indirectStrength;
			available->useDirectRR = directRR;
			available->rasterBlend = a_rasterBlend;
			available->normalMode = std::min(a_normalMode, 2u);
			available->directTraceMode = fsrIndependent ? 5u : nativeDirectTest ? 5u :
                std::min(a_directResolutionMode, 7u);
			available->indirectTraceMode = std::min(a_indirectResolutionMode, 7u);
			available->directRRResolutionMode = effectiveRRMode;
			available->rrMaterialPolicy = a_rrMaterialIsolation ? (0x80000000u |
                (uint32_t(std::clamp(a_rrGlossyRoughness, 0.04f, 1.0f) * 65535.0f) << 8u)) : 0u;
            if (fsrIndependent) {
                available->rrMaterialPolicy |= 0x20000000u; // FSR raw signal mode
                if (composer.composition.fsrHybrid.clampRadiance)
                    available->rrMaterialPolicy |= 0x40000000u;
                available->falloffParameters.w = std::clamp(
                    composer.composition.fsrHybrid.radianceClamp, 1.0f, 65504.0f);
            }
			available->preparedLights = a_allPreparedLights ? std::vector<PreparedPointLight>(a_preparedLights.begin(), a_preparedLights.end()) :
				std::vector<PreparedPointLight>{ a_preparedLights.empty() ? PreparedPointLight{} : a_preparedLights.front() };
			if (a_allPreparedLights) available->lightIdentities.assign(a_lightIdentities.begin(), a_lightIdentities.end());
			else available->lightIdentities = { { a_lightReference, a_lightBase } };
			available->bytes = textureBytes * 2 + lightBytes + signalBytes + visibilityBytes + maskBytes + exportBytes + temporalBytes + hybridBytes + nativeGIBytes + nativeDirectBytes + kFrameOverheadBytes;
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = size[0]; desc.Height = size[1];
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			if (!available->receivers) available->receivers = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "RTDirectLighting::Receivers");
			if (!available->output) available->output = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "RTDirectLighting::BlockerDistance");
			if (wantHybridInputs) {
                const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
                const auto sourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
                    DXGI_FORMAT_R16G16B16A16_FLOAT, size[0], size[1], 1, 1, 1, 0,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                if (!available->hybridShadowedSource) {
                    DX::ThrowIfFailed(backend.d3d12Device->CreateCommittedResource(
                        &heap, D3D12_HEAP_FLAG_NONE, &sourceDesc, D3D12_RESOURCE_STATE_COMMON,
                        nullptr, IID_PPV_ARGS(available->hybridShadowedSource.put())));
                    available->hybridShadowedSource->SetName(L"RTComposer::HybridShadowedSource");
                }
                if (!available->hybridLinearDepth) {
                    DX::ThrowIfFailed(backend.d3d12Device->CreateCommittedResource(
                        &heap, D3D12_HEAP_FLAG_NONE, &linearDepthDesc, D3D12_RESOURCE_STATE_COMMON,
                        nullptr, IID_PPV_ARGS(available->hybridLinearDepth.put())));
                    available->hybridLinearDepth->SetName(L"RTComposer::HybridLinearDepth");
                }
                static constexpr std::array names{
                    "RTComposer::HybridScene", "RTComposer::HybridDepth",
                    "RTComposer::HybridNormalRoughness", "RTComposer::HybridAlbedo",
                    "RTComposer::HybridReflectance", "RTComposer::HybridMotion"
                };
                for (uint32_t i = 0; i < kHybridInputCount; ++i) {
                    desc.Format = i == 1u ? DXGI_FORMAT_R32G32B32A32_FLOAT :
                        DXGI_FORMAT_R16G16B16A16_FLOAT;
                    if (!available->hybridInputs[i])
                        available->hybridInputs[i] = std::make_unique<WrappedResource>(
                            desc, backend.d3d11Device.get(), backend.d3d12Device.get(), names[i]);
                }
                desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                hybridInputLastBytes = hybridBytes;
            }
            if (wantShadowTemporal) {
                desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                if (!available->shadowMotion)
                    available->shadowMotion = std::make_unique<WrappedResource>(
                        desc, backend.d3d11Device.get(), backend.d3d12Device.get(),
                        "RTComposer::ShadowTemporalMotion");
                desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            }
			if (a_sceneTarget) {
				const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
				if (wantEarlyMask && !available->visibilityBits) {
					DX::ThrowIfFailed(backend.d3d12Device->CreateCommittedResource(
						&heap, D3D12_HEAP_FLAG_NONE, &visibilityDesc, D3D12_RESOURCE_STATE_COMMON,
						nullptr, IID_PPV_ARGS(available->visibilityBits.put())));
					available->visibilityBits->SetName(L"RTDirectLighting::PerLightVisibility");
				}
				if (wantEarlyMask && !available->shadowMask) {
					DX::ThrowIfFailed(backend.d3d12Device->CreateCommittedResource(
						&heap, D3D12_HEAP_FLAG_NONE, &maskDesc, D3D12_RESOURCE_STATE_COMMON,
						nullptr, IID_PPV_ARGS(available->shadowMask.put())));
					available->shadowMask->SetName(L"RTDirectLighting::WeightedShadowMask");
				}
                if (wantShadowTemporal && !available->shadowTemporal) {
                    DX::ThrowIfFailed(backend.d3d12Device->CreateCommittedResource(
                        &heap, D3D12_HEAP_FLAG_NONE, &maskDesc, D3D12_RESOURCE_STATE_COMMON,
                        nullptr, IID_PPV_ARGS(available->shadowTemporal.put())));
                    available->shadowTemporal->SetName(L"RTComposer::TemporalShadowMask");
                }
				if (wantExport && !available->shadowMaskExport) {
					desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
					available->shadowMaskExport = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "RTDirectLighting::ShadowMaskDX11Export");
					desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
				}
			}
			if (directRR || a_indirectRays || nativeDirectTest) {
				desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				static constexpr std::array names{ "RTDirectLighting::DirectDiffuse", "RTDirectLighting::DirectSpecular", "RTDirectLighting::DirectNormalRoughness", "RTDirectLighting::DirectSpecularAlbedo", "RTDirectLighting::DirectDiffuseAlbedo", "RTDirectLighting::IndirectDiffuse", "RTDirectLighting::IndirectSpecular" };
				for (uint32_t i = 0; i < kDirectSignalCount; ++i)
					if (!available->directSignals[i]) available->directSignals[i] = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), names[i]);
			}
			desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			desc.Width = kLightSnapshotWidth; desc.Height = 2;
			if (!available->light) available->light = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "RTDirectLighting::EvaluatedLightSnapshot");
			// Upload immutable prepared lights to the existing GPU snapshot layout.
			// D3D11 receiver preparation only stamps per-pixel depth and room mappings.
			std::array<std::array<float, 4>, kLightSnapshotWidth * 2> snapshot{};
			for (size_t lightIndex = 0; lightIndex < available->preparedLights.size() && lightIndex < kMaximumDirectLights; ++lightIndex) {
				const auto& item = available->preparedLights[lightIndex];
				auto* record = snapshot.data() + lightIndex * kLightSnapshotTexels;
				record[1] = { item.radius, item.radius > 0 ? 1.0f / item.radius : 0.0f, item.radius * 0.1f, item.sourceRadius };
				record[2] = { item.position.x, item.position.y, item.position.z, 1 };
				record[5] = { 0, item.supported ? 0.0f : 7.0f, item.supported ? 1.0f : 0.0f, 0 };
				record[6] = { 0, 0, 0, item.supported ? 1.0f : 0.0f };
				record[7] = { item.radiance.x, item.radiance.y, item.radiance.z, item.supported ? 1.0f : 0.0f };
			}
			backend.d3d11Context->UpdateSubresource(available->light->resource11, 0, nullptr,
				snapshot.data(), kLightSnapshotWidth * sizeof(snapshot[0]), 0);
			PrepareReceivers(*available, a_nativeSize, lights, a_roomMetadata);
			Record(*available, a_lightDirection, a_localLight);
			if (direct) ++directSubmissions;
			if (a_sceneTarget) CompositeScene(*available, a_sceneTarget);
			localLightMode = a_localLight;
			sourceLightCount = uint32_t(available->preparedLights.size());
			sourceLightFrame = a_frame;
			sourceReference = a_lightReference;
			sourceBase = a_lightBase;
			sourceRoomGeneration = lights.roomGeneration;
			sourceRoomMetadata = a_roomMetadata != nullptr;
			allPreparedLights = a_allPreparedLights;
			requestedLightCount = uint32_t(available->preparedLights.size());
		} catch (const std::exception& error) {
			failure = error.what(); previewValid = false;
			if (!available->submitted) *available = {};
		}
	}

    DirectionalTrace::TransportSignalsPublication DirectionalTrace::GetTransportSignals(uint32_t frameNumber) const
    {
        // A publication is never a scene-composition permission. The alpha
        // sentinel is independent for each family: -2 unsampled, -1 miss.
        // The owning frame and the completion fence must outlive consumers.
        for (const auto& frame : frames) {
            if (!frame.submitted || frame.number != frameNumber ||
                frame.generation != generation || !frame.fence ||
                !completion.fence12 || frame.transportTier < 2u ||
                frame.transportTier > 5u || frame.indirectRays == 0u ||
                !frame.directSignals[5] || !frame.directSignals[6]) continue;
            return { frame.directSignals[5]->resource.get(),
                frame.directSignals[6]->resource.get(),
                completion.fence12.get(), frame.fence, frame.number,
                frame.transportTier, frame.indirectTraceMode, frame.size };
        }
        return {};
    }

    DirectionalTrace::NativeGIOutputsPublication DirectionalTrace::GetNativeGIOutputs(uint32_t frameNumber) const
    {
        for (const auto& frame : frames) {
            if (!frame.submitted || !frame.nativeGI.valid || frame.nativeGIFrame != frameNumber ||
                frame.generation != generation || !frame.fence || !completion.fence12) continue;
            const auto& final = frame.transportTier >= 4u ? frame.postHybridGI : frame.nativeGI;
            if (!final.valid) continue;
            NativeGIOutputsPublication result{};
            for (uint32_t i = 0; i < NativeHybridGI::kOutputs; ++i)
                result.outputs[i] = final.textures[i].get();
            result.sampling = final.sampling;
            result.rrGuidesValid = final.rrGuidesValid;
            result.rrValid = final.rrValid;
            result.postValid = final.postValid;
            if (result.rrValid)
                for (uint32_t i = 0; i < result.rrOutputs.size(); ++i)
                    result.rrOutputs[i] = final.rrOutputs[i].get();
            if (result.postValid)
                for (uint32_t i = 0; i < result.reconstructed.size(); ++i)
                    result.reconstructed[i] = final.nativeFinal[i];
            if (result.rrGuidesValid)
                for (uint32_t i = 0; i < result.rrGuides.size(); ++i)
                    result.rrGuides[i] = final.rrGuides[i].get();
            result.composedValid = final.composedValid &&
                bool(final.composedScene);
            result.composedScene = result.composedValid ?
                final.composedScene->resource.get() : nullptr;
            result.completionFence = completion.fence12.get();
            result.completionValue = frame.fence;
            result.size = frame.size;
            return result;
        }
        return {};
    }

	DirectionalTrace::ShadowedSourcePublication DirectionalTrace::GetShadowedSource(uint32_t frameNumber) const
    {
        for (const auto& frame : frames)
            if (frame.submitted && frame.hybridShadowedSourceReady && frame.number == frameNumber &&
                frame.generation == generation && frame.fence && completion.fence12)
                return { frame.hybridShadowedSource.get(), frame.hybridLinearDepth.get(),
                    completion.fence12.get(), frame.fence, frame.size };
        return {};
    }

	DirectionalTrace::HybridInputsPublication DirectionalTrace::GetHybridInputs(uint32_t frameNumber) const
    {
        for (const auto& frame : frames) {
            if (!frame.submitted || !frame.hybridInputsReady || frame.number != frameNumber ||
                frame.generation != generation || !frame.fence || !completion.fence12) continue;
            HybridInputsPublication publication{};
            for (uint32_t i = 0; i < kHybridInputCount; ++i)
                publication.inputs[i] = frame.hybridInputs[i] ? frame.hybridInputs[i]->resource.get() : nullptr;
            publication.completionFence = completion.fence12.get();
            publication.completionValue = frame.fence;
            publication.size = frame.size;
            return publication;
        }
        return {};
    }

	DirectionalTrace::ShadowMaskPublication DirectionalTrace::GetShadowMask(uint32_t a_frame) const
	{
		for (const auto& frame : frames) {
			if (frame.submitted && frame.number == a_frame && frame.generation == generation &&
				frame.fence && frame.shadowMask && frame.visibilityBits && completion.fence12) {
				return { EffectiveShadowMask(frame), frame.visibilityBits.get(),
					completion.fence12.get(), frame.fence, frame.size };
			}
		}
		return {};
	}

	json DirectionalTrace::GetDiagnostics() const
	{
		const auto materials = cachedBindings ? cachedBindings->materials : nullptr;
        const Frame* currentHybrid = nullptr;
        for (const auto& frame : frames)
            if (frame.submitted && frame.nativeGI.valid &&
                frame.generation == generation && (!currentHybrid ||
                frame.number > currentHybrid->number)) currentHybrid = &frame;
        const HybridSamplingLayout sampling = currentHybrid ?
            currentHybrid->nativeGI.sampling : HybridSamplingLayout{};
		return {
			{ "submissions", submissions }, { "completions", completions }, { "previewDrawSubmissions", previewDrawSubmissions },
			{ "nativeDirectPackDispatches", nativeDirectPackDispatches },
            { "nativeDirectRRSuccessfulFrames", nativeDirectRRSuccessfulFrames },
            { "nativeDirectRRFailedFrames", nativeDirectRRFailedFrames },
            { "nativeDirectRRFailure", nativeDirectRRFailure },
            { "nativeDirectRRComposeDispatches", nativeDirectRRComposeDispatches },
            { "nativeDirectRRComposeFailure", nativeDirectRRComposeFailure },
            { "nativeDirectRRPresentedFrames", nativeDirectRRPresentedFrames },
            { "nativeDirectRRLastReady", std::ranges::any_of(frames,
                [](const Frame& frame) { return frame.nativeDirectRRReady; }) },
            { "nativeDirectPackFailure", nativeDirectPackFailure },
            { "directRRDispatches", directRRDispatches }, { "directRRFrame", directRRFrame }, { "directRRBudgetDeferrals", directRRBudgetDeferrals },
			{ "rrDebugSource", rrFullscreenSource }, { "rrDebugSignal", rrSignalView }, { "rrDebugPreviewValid", rrDebugPreviewValid }, { "rrDebugPreviewFrame", rrDebugPreviewFrame },
			{ "rrDebugPreviewFailed", rrDebugFailed }, { "rrDebugStorageSize", rrDebugStorageSize },
			{ "directRR", directDenoiser.GetRayRegenerationDiagnostics() }, { "dxrIndirectRR", indirectDenoiser.GetRayRegenerationDiagnostics() },
			{ "rrSize", directRRSize }, { "rrCheckerboard", directRRCheckerboard }, { "bakedRoomCount", materials ? materials->roomCount : 0 }, { "bakedRoomsKnown", materials && materials->roomsKnown }, { "indirectCompositeSubmissions", indirectCompositeSubmissions },
			{ "previewValid", previewValid }, { "previewFrame", previewFrame }, { "previewSize", previewSize },
			{ "receiverStage", "opaque world lighting complete; before first-person camera rebasing" },
			{ "previewUnavailableLegend", "red: receiver; blue: ray miss; orange: unsupported material; magenta: no reference match/material unavailable; yellow: depth mismatch; cyan: unknown room; gray: room exclusion; dark purple: no table; brown: unresolved reference; pink: ambiguous reference; violet: unsupported light; white: invalid data" },
			{ "residentBytes", ResidentBytes() + MaterialResidentBytes() }, { "traceResidentBytes", ResidentBytes() },
			{ "materialView", previewMaterialView }, { "budgetDeferrals", budgetDeferrals }, { "busyFrames", busyFrames },
			{ "failure", failure }, { "minimumDistance", kRayMinimum }, { "maximumDistance", kRayMaximum }, { "angularRadius", localLightMode ? 0.0f : kSunAngularRadius },
			{ "lightMode", localLightMode ? "prepared-static" : "directional" },
			{ "allPreparedLights", allPreparedLights }, { "requestedLightCount", requestedLightCount }, { "maximumDirectLights", kMaximumDirectLights }, { "lightSelectionFailure", lightSelectionFailure },
			{ "directSubmissions", directSubmissions }, { "directTraceDispatches", directTraceDispatches }, { "indirectTraceDispatches", indirectTraceDispatches },
            { "advancedTransportDispatches", advancedTransportDispatches },
            { "advancedTransportSignalValidity", "independent diffuse/specular alpha: -2 unsampled, -1 sampled miss, >=0 sampled hit; fence-owned GPU-only" },
			{ "shadowMaskTraceDispatches", shadowMaskTraceDispatches },
            { "shadowMaskResolveDispatches", shadowMaskResolveDispatches },
            { "shadowTemporalDispatches", shadowTemporalDispatches },
            { "shadowTemporalReprojections", shadowTemporalReprojections },
            { "shadowTemporalHistoryWeight", 0.70f },
            { "shadowTemporalFailure", shadowTemporalFailure },
            { "hybridInputDispatches", hybridInputDispatches },
            { "hybridShadowedSourceDispatches", hybridSourceDispatches },
            { "nativeHybridGIDispatches", nativeHybridGI ? nativeHybridGI->GIDispatches() : uint64_t{0} },
            { "nativeHybridAODispatches", nativeHybridGI ? nativeHybridGI->AODispatches() : uint64_t{0} },
            { "hybridGIInputScene", hybridGIInputScene },
            { "hybridGIInputSceneFrame", hybridGIInputSceneFrame },
            { "nativeHybridEligibilityDispatches", nativeHybridGI ? nativeHybridGI->EligibilityDispatches() : uint64_t{0} },
            { "nativeHybridRRGuideDispatches", nativeHybridGI ? nativeHybridGI->GuideDispatches() : uint64_t{0} },
            { "nativeHybridReconstructionDispatches", nativeHybridGI ?
                nativeHybridGI->NativeReconstructionDispatches() : uint64_t{0} },
            { "nativeHybridTemporalDispatches", nativeHybridGI ?
                nativeHybridGI->NativeTemporalDispatches() : uint64_t{0} },
            { "firstPassRecordedRRFamilyMask", nativeHybridGI ?
                nativeHybridGI->LastRecordedRRFamilyMask() : 0u },
            { "firstPassRecordedNativeFamilyMask", nativeHybridGI ?
                nativeHybridGI->LastRecordedNativeFamilyMask() : 0u },
            { "firstPassRecordedReconstructionFrame", nativeHybridGI ?
                nativeHybridGI->LastRecordedReconstructionFrame() : UINT32_MAX },
            { "postHybridRecordedRRFamilyMask", postHybridGI ?
                postHybridGI->LastRecordedRRFamilyMask() : 0u },
            { "postHybridRecordedNativeFamilyMask", postHybridGI ?
                postHybridGI->LastRecordedNativeFamilyMask() : 0u },
            { "postHybridRecordedReconstructionFrame", postHybridGI ?
                postHybridGI->LastRecordedReconstructionFrame() : UINT32_MAX },
            { "nativeHybridTemporalFailure", nativeHybridGI ?
                nativeHybridGI->TemporalFailure() : std::string{} },
            { "nativeHybridRetainedHistoryResources", nativeHybridGI ?
                nativeHybridGI->RetainedNativeHistoryResources() : size_t{0} },
            { "nativeHybridRetainedHistoryBytes", nativeHybridGI ?
                nativeHybridGI->RetainedNativeHistoryBytes() : uint64_t{0} },
            { "postHybridTemporalDispatches", postHybridGI ?
                postHybridGI->NativeTemporalDispatches() : uint64_t{0} },
            { "postHybridTemporalFailure", postHybridGI ?
                postHybridGI->TemporalFailure() : std::string{} },
            { "postHybridRetainedHistoryBytes", postHybridGI ?
                postHybridGI->RetainedNativeHistoryBytes() : uint64_t{0} },
            { "nativeHybridReconstructionProvider", nativeHybridGI ?
                nativeHybridGI->ReconstructionProvider() : std::string{} },
            { "nativeHybridReconstructionFallbackReason", nativeHybridGI ?
                nativeHybridGI->ReconstructionFallbackReason() : std::string{} },
            { "postHybridReconstructionProvider", postHybridGI ?
                postHybridGI->ReconstructionProvider() : std::string{} },
            { "nativeHybridRRGuideFailure", nativeHybridGI ? nativeHybridGI->GuideFailure() : std::string{} },
            { "nativeHybridHistoryValidationDispatches", nativeHybridGI ? nativeHybridGI->HistoryValidationDispatches() : uint64_t{0} },
            { "nativeHybridHistoryFailure", nativeHybridGI ? nativeHybridGI->HistoryFailure() : std::string{} },
            { "nativeHybridLogicalGrid", sampling.logical },
            { "nativeHybridPackedGrid", sampling.packed },
            { "nativeHybridTracedPixels", sampling.tracedPixels },
            { "nativeHybridSamplePhase", sampling.representativePhase },
            { "nativeHybridSpatialScale", sampling.spatialScale },
            { "nativeHybridCheckerboardOrigin", sampling.checkerboardOrigin },
            { "nativeHybridEffectiveSamplingMode", static_cast<uint32_t>(sampling.effective) },
            { "nativeHybridGIFailure", nativeHybridGI ? nativeHybridGI->Failure() : std::string{} },
            { "nativeHybridCompositionDispatches", nativeHybridGI ? nativeHybridGI->CompositionDispatches() : uint64_t{0} },
            { "nativeHybridCompositionFailure", nativeHybridGI ? nativeHybridGI->CompositionFailure() : std::string{} },
            { "nativeHybridPostDispatches", nativeHybridGI ? nativeHybridGI->PostDispatches() : uint64_t{0} },
            { "nativeHybridBlurDispatches", nativeHybridGI ? nativeHybridGI->BlurDispatches() : uint64_t{0} },
            { "nativeHybridPostFailure", nativeHybridGI ? nativeHybridGI->PostFailure() : std::string{} },
            { "nativeHybridRRDispatches", globals::features::redstoneGI.GetDenoiser().NativeHybridDispatches() },
            { "nativeHybridRRResets", globals::features::redstoneGI.GetDenoiser().NativeHybridResets() },
            { "nativeHybridRRFailure", globals::features::redstoneGI.GetDenoiser().NativeHybridFailure() },
            { "nativeHybridPreviewSubmissions", nativeHybridPreviewSubmissions },
            { "nativeHybridPreviewFailure", nativeHybridPreviewFailure },
            { "nativeHybridGIStatus", "Production DX12 Hybrid-Shadowed; GPU RR or full-native raw, raster preserved on failed frame" },
            { "advancedRRDispatches", advancedDenoiser ?
                advancedDenoiser->NativeHybridDispatches() : uint64_t{0} },
            { "advancedRRFailure", advancedDenoiser ?
                advancedDenoiser->NativeHybridFailure() : std::string{} },
            { "advancedComposeSubmissions", advancedComposeSubmissions },
            { "postHybridComposeSubmissions", postHybridComposeSubmissions },
            { "advancedPresentedFrames", advancedPresentedFrames },
            { "advancedRasterFallbackFrames", advancedRasterFallbackFrames },
            { "advancedComposeFailure", advancedComposeFailure },
            { "advancedPackDispatches", nativeHybridGI ? nativeHybridGI->AdvancedPackDispatches() : uint64_t{0} },
            { "nativeHybridFallbackFrames", nativeHybridFallbackFrames },
            { "hybridShadowedSourceLastFrame", hybridSourceLastFrame },
            { "hybridShadowedSourceFailure", hybridSourceFailure },
            { "hybridShadowedSourceStage", "5A-preparation; native shadowed linear radiance and linear view depth" },
            { "hybridInputLastFrame", hybridInputLastFrame },
            { "hybridInputLastBytes", hybridInputLastBytes },
            { "hybridInputBudgetDeferrals", hybridInputBudgetDeferrals },
            { "hybridInputFailure", hybridInputFailure },
            { "hybridInputStage", "DX11 raster capture / DX12 shared inputs / producer fence; native GI consumed on DX12" },
			{ "shadowMaskFormat", "R16G16_FLOAT (visibility, confidence)" }, { "perLightVisibilityFormat", "R32G32_UINT (visible, valid bitsets)" }, { "sceneCompositeSubmissions", sceneCompositeSubmissions }, { "sceneCompositeFrame", sceneCompositeFrame },
			{ "directScope", "selected light or complete bounded prepared point-light set; supported depth-matched PBR surfaces; per-light diffuse/specular, emission once; no native CPU bypass" },
			{ "sourceLightCount", sourceLightCount }, { "sourceLightFrame", sourceLightFrame }, { "unavailableLightFrames", unavailableLightFrames },
			{ "bindingCreations", bindingCreations }, { "bindingDeferrals", bindingDeferrals }, { "cachedSceneBindings", bool(cachedBindings) },
			{ "bindingsLoading", pendingBindings && pendingBindings->busy.load(std::memory_order_acquire) },
			{ "texturePools", materials ? materials->pools.size() : 0 }, { "uniqueTextures", materials ? materials->textures : 0 },
			{ "materialTextureBytes", materials ? materials->bytes : 0 }, { "materialTableBytes", materials ? materials->tableBytes : 0 },
			{ "readySourceMaterials", materials ? materials->readyMaterials : 0 }, { "rejectedSourceMaterials", materials ? materials->rejectedMaterials : 0 },
			{ "materialFailure", materials ? materials->failure : "" }, { "materialCompatibilityValidated", false },
			{ "sourceReference", sourceReference }, { "sourceBase", sourceBase }, { "sourceRoomGeneration", sourceRoomGeneration }, { "sourceRoomMetadata", sourceRoomMetadata },
			{ "lightMatch", "prepared static baseline; unsupported overrides/PortalStrict or live changes fall back to raster" },
			{ "radianceProduced", false }, { "shadowMapInputs", false },
			{ "blockerDistance", "nearest accepted triangle along the sampled unit direction; exposed sentinel 65504" },
			{ "coverageValid", false }, { "sceneLightingConsumed", false }, { "rrSubmitted", false }
		};
	}
}

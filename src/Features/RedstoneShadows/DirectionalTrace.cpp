#include "DirectionalTrace.h"

#include "RedstoneShadows.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Features/RedstoneGI/RedstoneGI.h"
#include "Deferred.h"
#include "Utils/RayRegenerationSettings.h"

#include <fstream>
#include <BS_thread_pool.hpp>

namespace RedstoneShadow
{
	bool DirectionalTrace::Initialize(const GraphicsBackend& a_backend)
	{
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
				producer.Create(backend.d3d12Device.get(), backend.d3d11Device.get(), "RedstoneShadows::ReceiverProducer");
				completion.Create(backend.d3d12Device.get(), backend.d3d11Device.get(), "RedstoneShadows::TraceCompletion");
			}
			CreatePipeline();
			prepare.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\RedstoneShadows\\prepareReceivers.cs.hlsl", {}, "cs_5_0")));
			display.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\RedstoneShadows\\previewDirectional.cs.hlsl", {}, "cs_5_0")));
			if (!prepare || !display) throw std::runtime_error("Directional receiver/preview shader unavailable");
			return true;
		} catch (const std::exception& error) { failure = error.what(); return false; }
	}

	DirectionalTrace::~DirectionalTrace()
	{
		const auto done = completion.fence12 ? completion.fence12->GetCompletedValue() : 0;
		const auto retired = producer.fence12 ? producer.fence12->GetCompletedValue() : 0;
		bool inFlight = false;
		for (auto& frame : frames) {
			if (!frame.submitted || (frame.fence && frame.fence <= done && (!frame.retirement || frame.retirement <= retired))) continue;
			inFlight = true;
			(void)new Frame(std::move(frame));
		}
		if (inFlight) {
			(void)producer.fence11.detach(); (void)producer.fence12.detach();
			(void)completion.fence11.detach(); (void)completion.fence12.detach();
		}
	}

	void DirectionalTrace::Reset(bool a_releaseBindings)
	{
		++generation;
		previewValid = false;
		rrDebugPreviewValid = false;
		lightSelectionFailure.clear();
		for (auto& frame : frames) if (!frame.submitted) frame = {};
		if (a_releaseBindings) cachedBindings.reset();
	}

	void DirectionalTrace::ReloadShaders()
	{
		Reset();
		pipeline = nullptr;
		root = nullptr;
		prepare = nullptr;
		display = nullptr;
		composite = nullptr;
		prepareDirectGuides = nullptr;
		packRRSignals = nullptr;
		rrDebugShader = nullptr;
		rrDebugFailed = false;
		failure.clear();
	}

	void DirectionalTrace::CreatePipeline()
	{
		if (pipeline) return;
		std::ifstream file("Data/Shaders/RedstoneShadows/directional.dxil", std::ios::binary | std::ios::ate);
		if (!file) throw std::runtime_error("Packaged directional.dxil unavailable; build the RedstoneShadowDXIL target");
		const std::streamoff size = file.tellg();
		constexpr std::streamoff kMaximumShaderBytes = 16 * 1024 * 1024;
		if (size <= 0 || size > kMaximumShaderBytes) throw std::runtime_error("Invalid directional DXIL size");
		std::vector<char> bytecode(static_cast<size_t>(size));
		file.seekg(0);
		if (!file.read(bytecode.data(), std::streamsize(bytecode.size()))) throw std::runtime_error("Cannot read directional DXIL");
		D3D12_DESCRIPTOR_RANGE ranges[6]{};
		ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
		ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1 + kDirectSignalCount, 0, 0, 0 };
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
		parameters[5].Constants = { 0, 0, 28 };
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
		root->SetName(L"RedstoneShadows::Directional Root");
		D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
		state.pRootSignature = root.get();
		state.CS = { bytecode.data(), bytecode.size() };
		DX::ThrowIfFailed(backend.d3d12Device->CreateComputePipelineState(&state, IID_PPV_ARGS(pipeline.put())));
		pipeline->SetName(L"RedstoneShadows::Directional Pipeline");
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
		result->heap->SetName(L"RedstoneShadows::SceneBindings");
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
		result->offsets->SetName(L"RedstoneShadows::ValidationOffsets");
		const D3D12_RANGE noRead{ 0, 0 };
		void* mapped = nullptr;
		DX::ThrowIfFailed(result->offsets->Map(0, &noRead, &mapped));
		std::memcpy(mapped, offsets.data(), offsets.size() * sizeof(uint32_t));
		result->offsets->Unmap(0, nullptr);
		return result;
	}

	void DirectionalTrace::PrepareReceivers(Frame& a_frame, const std::array<uint32_t, 2>& a_nativeSize, const LightLimitFixData::LightView& a_lights, ID3D11ShaderResourceView* a_roomMetadata)
	{
		CS_GPU_PASS("RedstoneShadows::PrepareReceivers");
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
			uint32_t requestedLightCount, padding[3]{};
			std::array<std::array<uint32_t, 4>, kMaximumDirectLights> identities{};
			std::array<std::array<uint32_t, 4>, 128> rooms{};
		};
		Grid grid{ a_frame.size[0], a_frame.size[1], a_nativeSize[0], a_nativeSize[1], a_frame.lightReference, a_frame.lightBase, a_lights.roomGeneration, a_roomMetadata ? 1u : 0u, a_lights.eyeOrigin, a_lights.count, uint32_t(a_frame.lightIdentities.size()) };
		for (size_t index = 0; index < a_frame.lightIdentities.size(); ++index)
			grid.identities[index] = { a_frame.lightIdentities[index][0], a_frame.lightIdentities[index][1], 0, 0 };
		for (uint32_t room = 0; room < grid.rooms.size(); ++room) grid.rooms[room] = { a_frame.roomReferences[room], a_lights.roomReferences[room], 0, 0 };
		static_assert(sizeof(Grid) == 64 + kMaximumDirectLights * 16 + 128 * 16);
		create(&grid, sizeof(grid), a_frame.grid, "RedstoneShadows::ReceiverGrid CB");
		create(&globals::game::frameBufferCached.nonVR, sizeof(globals::FrameBuffer), a_frame.camera, "RedstoneShadows::ReceiverCamera CB");
		winrt::com_ptr<ID3D11ComputeShader> shader;
		winrt::com_ptr<ID3D11ShaderResourceView> input, lightInput, roomInput;
		winrt::com_ptr<ID3D11UnorderedAccessView> output, lightOutput;
		winrt::com_ptr<ID3D11Buffer> oldGrid, oldCamera, oldShared, oldFeatures;
		auto* context = backend.d3d11Context.get();
		context->CSGetShader(shader.put(), nullptr, nullptr);
		context->CSGetShaderResources(0, 1, input.put());
		context->CSGetShaderResources(1, 1, lightInput.put());
		context->CSGetShaderResources(2, 1, roomInput.put());
		context->CSGetUnorderedAccessViews(0, 1, output.put());
		context->CSGetUnorderedAccessViews(1, 1, lightOutput.put());
		context->CSGetConstantBuffers(0, 1, oldGrid.put());
		context->CSGetConstantBuffers(12, 1, oldCamera.put());
		context->CSGetConstantBuffers(5, 1, oldShared.put());
		context->CSGetConstantBuffers(6, 1, oldFeatures.put());
		auto* depth = Util::GetCurrentSceneDepthSRV();
		if (!depth) throw std::runtime_error("Native receiver depth unavailable");
		auto* gridBuffer = a_frame.grid.get();
		auto* cameraBuffer = a_frame.camera.get();
		ID3D11ShaderResourceView* inputs[]{ depth, a_lights.records, a_roomMetadata };
		ID3D11UnorderedAccessView* outputs[]{ a_frame.receivers->uav, a_frame.light->uav };
		context->CSSetShaderResources(0, 3, inputs);
		context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
		context->CSSetConstantBuffers(0, 1, &gridBuffer);
		context->CSSetConstantBuffers(12, 1, &cameraBuffer);
		globals::state->BindSharedDataCS(context);
		context->CSSetShader(prepare.get(), nullptr, 0);
		a_frame.submitted = true;
		context->Dispatch((std::max({ a_frame.size[0], uint32_t(a_frame.lightIdentities.size()), 128u }) + 7) / 8, (a_frame.size[1] + 7) / 8, 1);
		ID3D11UnorderedAccessView* priorOutputs[]{ output.get(), lightOutput.get() };
		ID3D11ShaderResourceView* priorInputs[]{ input.get(), lightInput.get(), roomInput.get() };
		gridBuffer = oldGrid.get(); cameraBuffer = oldCamera.get();
		context->CSSetUnorderedAccessViews(0, 2, priorOutputs, nullptr);
		context->CSSetShaderResources(0, 3, priorInputs);
		context->CSSetConstantBuffers(0, 1, &gridBuffer);
		context->CSSetConstantBuffers(12, 1, &cameraBuffer);
		ID3D11Buffer* priorShared[]{ oldShared.get(), oldFeatures.get() };
		context->CSSetConstantBuffers(5, 2, priorShared);
		context->CSSetShader(shader.get(), nullptr, 0);
		const auto produced = producer.Next();
		DX::ThrowIfFailed(context->Signal(producer.fence11.get(), produced));
		context->Flush();
		DX::ThrowIfFailed(backend.commandQueue->Wait(producer.fence12.get(), produced));
	}

	void DirectionalTrace::Record(Frame& a_frame, const float3& a_direction, bool a_localLight)
	{
		CS_GPU_PASS("RedstoneShadows::DirectionalTraversal");
		a_frame.root = root;
		a_frame.pipeline = pipeline;
		if (a_frame.allocator) DX::ThrowIfFailed(a_frame.allocator->Reset());
		else DX::ThrowIfFailed(backend.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(a_frame.allocator.put())));
		if (a_frame.commands) DX::ThrowIfFailed(a_frame.commands->Reset(a_frame.allocator.get(), pipeline.get()));
		else DX::ThrowIfFailed(backend.d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a_frame.allocator.get(), pipeline.get(), IID_PPV_ARGS(a_frame.commands.put())));
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
		gpu.ptr += uint64_t(1 + kDirectSignalCount) * step; commands->SetComputeRootDescriptorTable(6, gpu);
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
		};
		const auto& origin = globals::game::frameBufferCached.GetCameraPosAdjust();
		const Constants constants{ a_frame.size[0], a_frame.size[1], a_frame.number, uint32_t(a_frame.scene->bottoms.size()), a_direction, kRayMaximum, { origin.x, origin.y, origin.z }, kRayMinimum, a_localLight ? 0.0f : kSunAngularRadius, { a_localLight ? 1u : 0u, a_frame.materialView, a_frame.allPreparedLights ? uint32_t(a_frame.lightIdentities.size()) : 0u }, a_frame.strengths, (a_frame.useDirectRR || a_frame.indirectRays) ? 1u : 0u, {}, { a_frame.indirectRays, a_frame.bindings->materials->roomCount, a_frame.bindings->materials->roomsKnown ? 1u : 0u, 0 } };
		static_assert(sizeof(Constants) == 28 * sizeof(uint32_t));
		commands->SetComputeRoot32BitConstants(5, 28, &constants, 0);
		std::vector<D3D12_RESOURCE_BARRIER> begin{
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.light->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.receivers->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.output->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
		};
		for (const auto& signal : a_frame.directSignals) if (signal) begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(signal->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		commands->ResourceBarrier(uint32_t(begin.size()), begin.data());
		commands->Dispatch((a_frame.size[0] + 7) / 8, (a_frame.size[1] + 7) / 8, 1);
		const auto ordered = CD3DX12_RESOURCE_BARRIER::UAV(a_frame.output->resource.get());
		commands->ResourceBarrier(1, &ordered);
		std::vector<D3D12_RESOURCE_BARRIER> end{
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.light->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.receivers->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.output->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)
		};
		for (const auto& signal : a_frame.directSignals) if (signal) end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(signal->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
		commands->ResourceBarrier(uint32_t(end.size()), end.data());
		DX::ThrowIfFailed(commands->Close());
		ID3D12CommandList* lists[]{ commands };
		backend.commandQueue->ExecuteCommandLists(1, lists);
		a_frame.fence = ++nextCompletion;
		DX::ThrowIfFailed(backend.commandQueue->Signal(completion.fence12.get(), a_frame.fence));
		++submissions;
	}

	void DirectionalTrace::Consume(Frame& a_frame)
	{
		CS_GPU_PASS("RedstoneShadows::PreviewRawVisibility");
		if (previewSize != a_frame.size || !preview) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_frame.size[0]; desc.Height = a_frame.size[1];
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			preview = std::make_unique<Texture2D>(desc, "RedstoneShadows::Directional Preview");
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
		if (!completion.fence12 || !failure.empty()) return;
		const auto done = completion.fence12->GetCompletedValue();
		const auto retired = producer.fence12->GetCompletedValue();
		if (done == UINT64_MAX || retired == UINT64_MAX) { failure = "Directional device removed"; previewValid = false; return; }
		try {
			for (auto& frame : frames) {
				if (!frame.submitted || !frame.fence || frame.fence > done) continue;
				if (!frame.retirement && frame.generation == generation && display) { Consume(frame); ++completions; }
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
		CS_GPU_PASS("RedstoneShadows::DirectSceneComposite");
		auto* context = backend.d3d11Context.get();
		struct Settings { uint32_t indirect, directRR; float indirectStrength; uint32_t useDXRIndirect; float redstoneBlend; float padding[3]{}; };
		if (!lightingCompositeSettings) lightingCompositeSettings = std::make_unique<ConstantBuffer>(ConstantBufferDesc<Settings>(), "RedstoneShadows::LightingComposite CB");
		winrt::com_ptr<ID3D11ComputeShader> shader;
		std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 15> inputs;
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
		globals::state->BindSharedDataCS(context);
		auto* cameraBuffer = a_frame.camera.get();
		context->CSSetConstantBuffers(12, 1, &cameraBuffer);
		const uint32_t rrFlags = DenoiseDirect(a_frame);
		PrepareRRPreview(a_frame, rrFlags);
		const bool directRR = (rrFlags & 1u) != 0;
		auto& indirect = globals::features::redstoneGI;
		const bool useIndirect = a_frame.useIndirect && indirect.HasCurrentIndirectLighting();
		if (useIndirect) indirect.BindCompositeResources();
		lightingCompositeSettings->Update(Settings{ useIndirect ? 1u : 0u, rrFlags, a_frame.indirectStrength, a_frame.indirectRays != 0 ? 1u : 0u, globals::features::redstoneShadows.settings.RedstoneBlend });
		auto* cb = lightingCompositeSettings->CB();
		context->CSSetConstantBuffers(0, 1, &cb);
		const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
		ID3D11ShaderResourceView* reads[]{ a_frame.output->srv, Util::AsReal(targets[NORMALROUGHNESS].SRV),
			directRR ? a_frame.directSignals[0]->srv : nullptr, directRR ? a_frame.directSignals[1]->srv : nullptr,
			directRR ? directRRTextures[2]->srv.get() : nullptr, directRR ? directRRTextures[3]->srv.get() : nullptr, directRR ? a_frame.directSignals[3]->srv : nullptr,
			a_frame.indirectRays ? a_frame.directSignals[5]->srv : nullptr, a_frame.indirectRays ? a_frame.directSignals[6]->srv : nullptr,
			(rrFlags & 2u) ? directRRTextures[4]->srv.get() : nullptr, (rrFlags & 2u) ? directRRTextures[5]->srv.get() : nullptr,
			rrFlags ? directRRTextures[0]->srv.get() : nullptr, rrFlags ? directRRTextures[10]->srv.get() : nullptr, a_frame.receivers->srv,
			rrFlags ? a_frame.directSignals[2]->srv : nullptr };
		context->CSSetShaderResources(0, 15, reads);
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


	void DirectionalTrace::Dispatch(std::shared_ptr<const TopLevel> a_scene, const std::array<uint32_t, 2>& a_nativeSize, const float3& a_lightDirection, uint32_t a_frame, uint64_t a_budgetBytes, bool a_localLight, const LightLimitFixData::LightView& a_lights, uint32_t a_materialView, ID3D11UnorderedAccessView* a_sceneTarget, uint32_t a_lightReference, uint32_t a_lightBase, ID3D11ShaderResourceView* a_roomMetadata, std::span<const std::array<uint32_t, 2>> a_lightIdentities, bool a_allPreparedLights, float4 a_strengths, bool a_useIndirect, float a_indirectStrength, bool a_useDirectRR, uint32_t a_indirectRays, std::span<const uint32_t> a_roomReferences)
	{
		CS_GPU_PASS("RedstoneShadows::DirectionalDispatch");
		const bool direct = a_materialView >= kFirstDirectView;
		lightSelectionFailure.clear();
		if (a_allPreparedLights && (!direct || a_lightIdentities.empty() || a_lightIdentities.size() > kMaximumDirectLights)) {
			lightSelectionFailure = "All-prepared-light mode requires 1..32 prepared lights and a direct-light view";
			previewValid = false;
			return;
		}
		if (a_sceneTarget && (a_materialView != kCombinedDirectView || (sceneCompositeSubmissions && sceneCompositeFrame == a_frame))) return;
		const bool validLights = a_lights.records && a_lights.count && a_lights.count <= LightLimitFixData::kMaximumLights && a_lights.frame == a_frame &&
			std::all_of(a_lights.eyeOrigin.begin(), a_lights.eyeOrigin.end(), [](float value) { return std::isfinite(value); });
		const auto lights = validLights ? a_lights : LightLimitFixData::LightView{};
		if (a_localLight && (!a_materialView || direct) && !validLights) ++unavailableLightFrames;
		if (!pipeline || !failure.empty() || !a_scene || a_scene->instances.empty() || a_scene->instances.size() > kMaximumInstances || !a_nativeSize[0] || !a_nativeSize[1]) return;
		if (a_nativeSize[0] > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || a_nativeSize[1] > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
			!std::isfinite(a_lightDirection.x) || !std::isfinite(a_lightDirection.y) || !std::isfinite(a_lightDirection.z)) return;
		Frame* available = nullptr;
		for (auto& frame : frames) if (!frame.scene) { available = &frame; break; }
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
		const bool directRR = a_sceneTarget && a_useDirectRR && RayRegeneration::GetSettings().enabled;
		const bool reuse = available->size == size && available->useDirectRR == directRR && available->indirectRays == a_indirectRays && available->receivers && available->output && available->light;
		constexpr uint64_t kFrameOverheadBytes = 1024 * 1024;
		const auto textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, size[0], size[1], 1, 1, 1, 0,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
		const auto previewDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, size[0], size[1], 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		const auto textureBytes = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &textureDesc).SizeInBytes;
		const auto lightDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, kLightSnapshotWidth, 2, 1, 1, 1, 0,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
		const auto lightBytes = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &lightDesc).SizeInBytes;
		const auto nextPreviewBytes = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &previewDesc).SizeInBytes;
		if (textureBytes == UINT64_MAX || lightBytes == UINT64_MAX || nextPreviewBytes == UINT64_MAX) { failure = "Directional texture allocation unsupported"; return; }
		const uint64_t signalBytes = (directRR || a_indirectRays) ? nextPreviewBytes * kDirectSignalCount : 0;
		const uint64_t rrInteropReserve = directRR ? uint64_t(std::max(1u, size[0] >> RayRegeneration::GetSettings().resolutionMode)) * std::max(1u, size[1] >> RayRegeneration::GetSettings().resolutionMode) * 256 + kFrameOverheadBytes : 0;
		const uint64_t required = (reuse ? 0 : textureBytes * 2 + lightBytes + signalBytes + kFrameOverheadBytes) + (a_sceneTarget ? 0 : nextPreviewBytes) + bindingBytes + rrInteropReserve;
		const uint64_t retainedBytes = ResidentBytes() - (reuse ? 0 : available->bytes);
		if (required > a_budgetBytes || retainedBytes > a_budgetBytes - required) {
			if (directRR) {
				++directRRBudgetDeferrals;
				Dispatch(a_scene, a_nativeSize, a_lightDirection, a_frame, a_budgetBytes, a_localLight, a_lights, a_materialView, a_sceneTarget,
					a_lightReference, a_lightBase, a_roomMetadata, a_lightIdentities, a_allPreparedLights, a_strengths, a_useIndirect, a_indirectStrength, false, a_indirectRays, a_roomReferences);
			} else ++budgetDeferrals;
			return;
		}
		try {
			if (!reuse) *available = {};
			if (a_sceneTarget && !composite) {
				composite.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\RedstoneShadows\\compositeDirect.cs.hlsl", {}, "cs_5_0")));
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
			available->materialView = std::min(a_materialView, kCombinedDirectView);
			available->generation = generation;
			available->lightReference = a_lightReference;
			available->lightBase = a_lightBase;
			available->allPreparedLights = a_allPreparedLights;
			available->strengths = a_strengths;
			available->useIndirect = a_useIndirect && a_indirectRays == 0;
			available->indirectRays = std::min(a_indirectRays, 8u);
			available->roomReferences.fill(0);
			std::copy_n(a_roomReferences.begin(), std::min(a_roomReferences.size(), available->roomReferences.size()), available->roomReferences.begin());
			available->indirectStrength = a_indirectStrength;
			available->useDirectRR = directRR;
			if (a_allPreparedLights) available->lightIdentities.assign(a_lightIdentities.begin(), a_lightIdentities.end());
			else available->lightIdentities = { { a_lightReference, a_lightBase } };
			available->bytes = textureBytes * 2 + lightBytes + signalBytes + kFrameOverheadBytes;
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = size[0]; desc.Height = size[1];
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			if (!available->receivers) available->receivers = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "RedstoneShadows::Receivers");
			if (!available->output) available->output = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "RedstoneShadows::BlockerDistance");
			if (directRR || a_indirectRays) {
				desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				static constexpr std::array names{ "RedstoneShadows::DirectDiffuse", "RedstoneShadows::DirectSpecular", "RedstoneShadows::DirectNormalRoughness", "RedstoneShadows::DirectSpecularAlbedo", "RedstoneShadows::DirectDiffuseAlbedo", "RedstoneShadows::IndirectDiffuse", "RedstoneShadows::IndirectSpecular" };
				for (uint32_t i = 0; i < kDirectSignalCount; ++i)
					if (!available->directSignals[i]) available->directSignals[i] = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), names[i]);
			}
			desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			desc.Width = kLightSnapshotWidth; desc.Height = 2;
			if (!available->light) available->light = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "RedstoneShadows::EvaluatedLightSnapshot");
			PrepareReceivers(*available, a_nativeSize, lights, a_roomMetadata);
			Record(*available, a_lightDirection, a_localLight);
			if (direct) ++directSubmissions;
			if (a_sceneTarget) CompositeScene(*available, a_sceneTarget);
			localLightMode = a_localLight;
			sourceLightCount = lights.count;
			sourceLightFrame = lights.frame;
			sourceReference = a_lightReference;
			sourceBase = a_lightBase;
			sourceRoomGeneration = lights.roomGeneration;
			sourceRoomMetadata = a_roomMetadata != nullptr;
			allPreparedLights = a_allPreparedLights;
			requestedLightCount = uint32_t(available->lightIdentities.size());
		} catch (const std::exception& error) {
			failure = error.what(); previewValid = false;
			if (!available->submitted) *available = {};
		}
	}

	json DirectionalTrace::GetDiagnostics() const
	{
		const auto materials = cachedBindings ? cachedBindings->materials : nullptr;
		return {
			{ "submissions", submissions }, { "completions", completions }, { "previewDrawSubmissions", previewDrawSubmissions },
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
			{ "lightMode", localLightMode ? "evaluated-local" : "directional" },
			{ "allPreparedLights", allPreparedLights }, { "requestedLightCount", requestedLightCount }, { "maximumDirectLights", kMaximumDirectLights }, { "lightSelectionFailure", lightSelectionFailure },
			{ "directSubmissions", directSubmissions }, { "sceneCompositeSubmissions", sceneCompositeSubmissions }, { "sceneCompositeFrame", sceneCompositeFrame },
			{ "directScope", "selected light or complete bounded prepared point-light set; supported depth-matched PBR surfaces; per-light diffuse/specular, emission once; no native CPU bypass" },
			{ "sourceLightCount", sourceLightCount }, { "sourceLightFrame", sourceLightFrame }, { "unavailableLightFrames", unavailableLightFrames },
			{ "bindingCreations", bindingCreations }, { "bindingDeferrals", bindingDeferrals }, { "cachedSceneBindings", bool(cachedBindings) },
			{ "bindingsLoading", pendingBindings && pendingBindings->busy.load(std::memory_order_acquire) },
			{ "texturePools", materials ? materials->pools.size() : 0 }, { "uniqueTextures", materials ? materials->textures : 0 },
			{ "materialTextureBytes", materials ? materials->bytes : 0 }, { "materialTableBytes", materials ? materials->tableBytes : 0 },
			{ "readySourceMaterials", materials ? materials->readyMaterials : 0 }, { "rejectedSourceMaterials", materials ? materials->rejectedMaterials : 0 },
			{ "materialFailure", materials ? materials->failure : "" }, { "materialCompatibilityValidated", false },
			{ "sourceReference", sourceReference }, { "sourceBase", sourceBase }, { "sourceRoomGeneration", sourceRoomGeneration }, { "sourceRoomMetadata", sourceRoomMetadata },
			{ "lightMatch", "exact placed-reference and base identity; exactly one supported live record required" },
			{ "radianceProduced", false }, { "shadowMapInputs", false },
			{ "blockerDistance", "nearest accepted triangle along the sampled unit direction; exposed sentinel 65504" },
			{ "coverageValid", false }, { "sceneLightingConsumed", false }, { "rrSubmitted", false }
		};
	}
}

#include "DirectionalTrace.h"

#include "Globals.h"
#include "GpuPass.h"
#include "Utils/D3D.h"

#include <fstream>

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

	void DirectionalTrace::Reset()
	{
		++generation;
		previewValid = false;
	}

	void DirectionalTrace::ReloadShaders()
	{
		Reset();
		pipeline = nullptr;
		root = nullptr;
		prepare = nullptr;
		display = nullptr;
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
		D3D12_DESCRIPTOR_RANGE ranges[3]{};
		ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
		ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
		ranges[2] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaximumInstances, 0, 1, 0 };
		D3D12_ROOT_PARAMETER parameters[6]{};
		parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		parameters[0].Descriptor.ShaderRegister = 0;
		for (uint32_t i = 0; i < 3; ++i) {
			parameters[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[i + 1].DescriptorTable = { 1, &ranges[i] };
		}
		parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		parameters[4].Descriptor.ShaderRegister = 2;
		parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[5].Constants = { 0, 0, 16 };
		const D3D12_ROOT_SIGNATURE_DESC desc{ uint32_t(std::size(parameters)), parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
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
		for (const auto& frame : frames) bytes += frame.bytes;
		return bytes;
	}

	void DirectionalTrace::PrepareReceivers(Frame& a_frame, const std::array<uint32_t, 2>& a_nativeSize)
	{
		CS_GPU_PASS("RedstoneShadows::PrepareReceivers");
		auto create = [&](const void* data, uint32_t bytes, winrt::com_ptr<ID3D11Buffer>& output, const char* name) {
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = bytes;
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			const D3D11_SUBRESOURCE_DATA initial{ data, 0, 0 };
			DX::ThrowIfFailed(backend.d3d11Device->CreateBuffer(&desc, &initial, output.put()));
			Util::SetResourceName(output.get(), name);
		};
		const uint32_t grid[]{ a_frame.size[0], a_frame.size[1], a_nativeSize[0], a_nativeSize[1] };
		create(grid, sizeof(grid), a_frame.grid, "RedstoneShadows::ReceiverGrid CB");
		create(&globals::game::frameBufferCached.nonVR, sizeof(globals::FrameBuffer), a_frame.camera, "RedstoneShadows::ReceiverCamera CB");
		winrt::com_ptr<ID3D11ComputeShader> shader;
		winrt::com_ptr<ID3D11ShaderResourceView> input;
		winrt::com_ptr<ID3D11UnorderedAccessView> output;
		winrt::com_ptr<ID3D11Buffer> oldGrid, oldCamera;
		auto* context = backend.d3d11Context.get();
		context->CSGetShader(shader.put(), nullptr, nullptr);
		context->CSGetShaderResources(0, 1, input.put());
		context->CSGetUnorderedAccessViews(0, 1, output.put());
		context->CSGetConstantBuffers(0, 1, oldGrid.put());
		context->CSGetConstantBuffers(12, 1, oldCamera.put());
		auto* depth = Util::GetCurrentSceneDepthSRV();
		if (!depth) throw std::runtime_error("Native receiver depth unavailable");
		auto* gridBuffer = a_frame.grid.get();
		auto* cameraBuffer = a_frame.camera.get();
		context->CSSetShaderResources(0, 1, &depth);
		context->CSSetUnorderedAccessViews(0, 1, &a_frame.receivers->uav, nullptr);
		context->CSSetConstantBuffers(0, 1, &gridBuffer);
		context->CSSetConstantBuffers(12, 1, &cameraBuffer);
		context->CSSetShader(prepare.get(), nullptr, 0);
		a_frame.submitted = true;
		context->Dispatch((a_frame.size[0] + 7) / 8, (a_frame.size[1] + 7) / 8, 1);
		auto* priorOutput = output.get();
		auto* priorInput = input.get();
		gridBuffer = oldGrid.get(); cameraBuffer = oldCamera.get();
		context->CSSetUnorderedAccessViews(0, 1, &priorOutput, nullptr);
		context->CSSetShaderResources(0, 1, &priorInput);
		context->CSSetConstantBuffers(0, 1, &gridBuffer);
		context->CSSetConstantBuffers(12, 1, &cameraBuffer);
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
		DX::ThrowIfFailed(backend.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(a_frame.allocator.put())));
		DX::ThrowIfFailed(backend.d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a_frame.allocator.get(), pipeline.get(), IID_PPV_ARGS(a_frame.commands.put())));
		const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kMaximumInstances + 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
		DX::ThrowIfFailed(backend.d3d12Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(a_frame.heap.put())));
		const auto step = backend.d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		auto cpu = a_frame.heap->GetCPUDescriptorHandleForHeapStart();
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
		std::vector<uint32_t> offsets;
		for (const auto& bottom : a_frame.scene->bottoms) {
			cpu.ptr += step;
			const auto& geometry = *bottom->geometry;
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R32_TYPELESS;
			srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Buffer.NumElements = uint32_t(geometry.buffer->GetDesc().Width / sizeof(uint32_t));
			srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			backend.d3d12Device->CreateShaderResourceView(geometry.buffer.get(), &srv, cpu);
			offsets.push_back(uint32_t(geometry.validationOffset));
		}
		const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		const auto upload = CD3DX12_RESOURCE_DESC::Buffer(offsets.size() * sizeof(uint32_t));
		DX::ThrowIfFailed(backend.d3d12Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &upload, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(a_frame.offsets.put())));
		a_frame.offsets->SetName(L"RedstoneShadows::ValidationOffsets");
		const D3D12_RANGE noRead{ 0, 0 };
		void* mapped = nullptr;
		DX::ThrowIfFailed(a_frame.offsets->Map(0, &noRead, &mapped));
		std::memcpy(mapped, offsets.data(), offsets.size() * sizeof(uint32_t));
		a_frame.offsets->Unmap(0, nullptr);
		ID3D12DescriptorHeap* heaps[]{ a_frame.heap.get() };
		auto* commands = a_frame.commands.get();
		commands->SetDescriptorHeaps(1, heaps);
		commands->SetComputeRootSignature(root.get());
		commands->SetComputeRootShaderResourceView(0, a_frame.scene->result->GetGPUVirtualAddress());
		auto gpu = a_frame.heap->GetGPUDescriptorHandleForHeapStart();
		commands->SetComputeRootDescriptorTable(1, gpu);
		gpu.ptr += step; commands->SetComputeRootDescriptorTable(2, gpu);
		gpu.ptr += step; commands->SetComputeRootDescriptorTable(3, gpu);
		commands->SetComputeRootShaderResourceView(4, a_frame.offsets->GetGPUVirtualAddress());
		struct Constants
		{
			uint32_t width, height, frame, count;
			float3 direction;
			float maximum;
			float3 origin;
			float minimum, radius;
			uint32_t padding[3]{};
		};
		const auto& origin = globals::game::frameBufferCached.GetCameraPosAdjust();
		const Constants constants{ a_frame.size[0], a_frame.size[1], a_frame.number, uint32_t(offsets.size()), a_direction, kRayMaximum, { origin.x, origin.y, origin.z }, kRayMinimum, a_localLight ? 0.0f : kSunAngularRadius, { a_localLight ? 1u : 0u, 0, 0 } };
		static_assert(sizeof(Constants) == 16 * sizeof(uint32_t));
		commands->SetComputeRoot32BitConstants(5, 16, &constants, 0);
		const D3D12_RESOURCE_BARRIER begin[]{
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.receivers->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.output->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
		};
		commands->ResourceBarrier(uint32_t(std::size(begin)), begin);
		commands->Dispatch((a_frame.size[0] + 7) / 8, (a_frame.size[1] + 7) / 8, 1);
		const auto ordered = CD3DX12_RESOURCE_BARRIER::UAV(a_frame.output->resource.get());
		commands->ResourceBarrier(1, &ordered);
		const D3D12_RESOURCE_BARRIER end[]{
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.receivers->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(a_frame.output->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)
		};
		commands->ResourceBarrier(uint32_t(std::size(end)), end);
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
				frame = {};
			}
		} catch (const std::exception& error) { failure = error.what(); previewValid = false; }
	}

	void DirectionalTrace::Dispatch(std::shared_ptr<const TopLevel> a_scene, const std::array<uint32_t, 2>& a_nativeSize, const float3& a_lightDirection, uint32_t a_frame, uint64_t a_budgetBytes, bool a_localLight)
	{
		CS_GPU_PASS("RedstoneShadows::DirectionalDispatch");
		if (!pipeline || !failure.empty() || !a_scene || a_scene->instances.empty() || a_scene->instances.size() > kMaximumInstances || !a_nativeSize[0] || !a_nativeSize[1]) return;
		if (a_nativeSize[0] > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || a_nativeSize[1] > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
			!std::isfinite(a_lightDirection.x) || !std::isfinite(a_lightDirection.y) || !std::isfinite(a_lightDirection.z)) return;
		Frame* available = nullptr;
		for (auto& frame : frames) if (!frame.scene) { available = &frame; break; }
		if (!available) { ++busyFrames; return; }
		const std::array<uint32_t, 2> size{ (a_nativeSize[0] + kGridDivisor - 1) / kGridDivisor, (a_nativeSize[1] + kGridDivisor - 1) / kGridDivisor };
		constexpr uint64_t kFrameOverheadBytes = 1024 * 1024;
		const auto textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, size[0], size[1], 1, 1, 1, 0,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
		const auto previewDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, size[0], size[1], 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		const auto textureBytes = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &textureDesc).SizeInBytes;
		const auto nextPreviewBytes = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &previewDesc).SizeInBytes;
		if (textureBytes == UINT64_MAX || nextPreviewBytes == UINT64_MAX) { failure = "Directional texture allocation unsupported"; return; }
		const uint64_t required = textureBytes * 2 + nextPreviewBytes + kFrameOverheadBytes;
		if (required > a_budgetBytes || ResidentBytes() > a_budgetBytes - required) { ++budgetDeferrals; return; }
		try {
			available->scene = std::move(a_scene);
			available->size = size;
			available->number = a_frame;
			available->generation = generation;
			available->bytes = textureBytes * 2 + kFrameOverheadBytes;
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = size[0]; desc.Height = size[1];
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			available->receivers = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "RedstoneShadows::Receivers");
			available->output = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "RedstoneShadows::BlockerDistance");
			PrepareReceivers(*available, a_nativeSize);
			Record(*available, a_lightDirection, a_localLight);
			localLightMode = a_localLight;
		} catch (const std::exception& error) {
			failure = error.what(); previewValid = false;
			if (!available->submitted) *available = {};
		}
	}

	json DirectionalTrace::GetDiagnostics() const
	{
		return {
			{ "submissions", submissions }, { "completions", completions }, { "previewDrawSubmissions", previewDrawSubmissions },
			{ "previewValid", previewValid }, { "previewFrame", previewFrame }, { "previewSize", previewSize },
			{ "residentBytes", ResidentBytes() }, { "budgetDeferrals", budgetDeferrals }, { "busyFrames", busyFrames },
			{ "failure", failure }, { "minimumDistance", kRayMinimum }, { "maximumDistance", kRayMaximum }, { "angularRadius", localLightMode ? 0.0f : kSunAngularRadius },
			{ "lightMode", localLightMode ? "baseline-local" : "directional" },
			{ "blockerDistance", "nearest accepted triangle along the sampled unit direction; exposed sentinel 65504" },
			{ "coverageValid", false }, { "sceneLightingConsumed", false }, { "rrSubmitted", false }
		};
	}
}

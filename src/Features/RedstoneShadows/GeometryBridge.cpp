#include "GeometryBridge.h"

#include "GpuPass.h"
#include "Utils/D3D.h"

namespace RedstoneShadow
{
	bool GeometryBridge::Initialize(const GraphicsBackend& a_backend)
	{
		if (failed) return false;
		if (completion && exportShader) return true;
		if (!completion) backend = a_backend;
		if (!backend.d3d11Device || !backend.d3d11Context || !backend.d3d12Device || !backend.commandQueue) return false;
		try {
			UINT support = 0;
			DX::ThrowIfFailed(backend.d3d11Device->CheckFormatSupport(DXGI_FORMAT_R32_UINT, &support));
			constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
			if ((support & required) != required) throw std::runtime_error("Integer geometry interop format unsupported");
			if (!completion) {
				producer.Create(backend.d3d12Device.get(), backend.d3d11Device.get(), "RedstoneShadows::GeometryProducer");
				DX::ThrowIfFailed(backend.d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(completion.put())));
				completion->SetName(L"RedstoneShadows::GeometryCompletion");
			}
			exportShader = nullptr;
			exportShader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\RedstoneShadows\\exportGeometry.cs.hlsl", {}, "cs_5_0")));
			if (!exportShader) throw std::runtime_error("Geometry export shader unavailable");
			return true;
		} catch (const std::exception& error) {
			failure = error.what();
			failed = true;
			return false;
		}
	}

	GeometryBridge::~GeometryBridge()
	{
		const auto done = completion ? completion->GetCompletedValue() : 0;
		bool inFlight = false;
		for (auto& transfer : transfers) {
			if (!transfer.submitted || (transfer.fence && transfer.fence <= done)) continue;
			inFlight = true;
			(void)transfer.texture.release();
			(void)new std::shared_ptr<Geometry>(std::move(transfer.geometry));
			(void)transfer.settings.detach();
			(void)transfer.vertices.detach();
			(void)transfer.indices.detach();
			(void)transfer.allocator.detach();
			(void)transfer.commands.detach();
		}
		if (inFlight) {
			(void)producer.fence11.detach();
			(void)producer.fence12.detach();
			(void)completion.detach();
		}
	}

	void GeometryBridge::Reset()
	{
		++generation;
		geometries.clear();
		currentMeshes.clear();
		snapshotGeneration = snapshotSweep = UINT64_MAX;
	}

	uint64_t GeometryBridge::ResidentBytes() const
	{
		uint64_t bytes = 0;
		for (const auto& [key, geometry] : geometries) bytes += geometry->allocationBytes + geometry->source->bytes;
		for (const auto& transfer : transfers)
			if (transfer.geometry) bytes += transfer.sharedBytes + transfer.geometry->allocationBytes + transfer.geometry->source->bytes;
		return bytes;
	}

	void GeometryBridge::Submit(Transfer& a_transfer, const std::shared_ptr<const Mesh>& a_mesh, uint64_t a_budgetBytes)
	{
		CS_GPU_PASS("RedstoneShadows::ExportGeometry");
		D3D11_BUFFER_DESC vertices{}, indices{};
		a_mesh->vertices->GetDesc(&vertices);
		a_mesh->indices->GetDesc(&indices);
		const uint64_t words = uint64_t(vertices.ByteWidth) / sizeof(uint32_t) + indices.ByteWidth / sizeof(uint16_t) + 1;
		const auto height = (words + kTextureWidth - 1) / kTextureWidth;
		if (!height || height > kMaximumTextureHeight) { ++budgetDeferrals; lastDeferral = "Mesh exceeds export texture height limit"; lastRequiredBytes = 0; lastBudgetBytes = a_budgetBytes; return; }
		const uint64_t sharedBytes = kTextureWidth * height * sizeof(uint32_t);
		const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(sharedBytes);
		const auto texture = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_UINT, kTextureWidth, uint32_t(height), 1, 1, 1, 0,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
		const auto bufferAllocation = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &buffer);
		const auto textureAllocation = backend.d3d12Device->GetResourceAllocationInfo(0, 1, &texture);
		if (bufferAllocation.SizeInBytes == UINT64_MAX || textureAllocation.SizeInBytes == UINT64_MAX) throw std::runtime_error("Geometry allocation layout unsupported");
		const uint64_t required = bufferAllocation.SizeInBytes + textureAllocation.SizeInBytes + a_mesh->bytes;
		if (required > a_budgetBytes || ResidentBytes() > a_budgetBytes - required) {
			++budgetDeferrals;
			lastDeferral = required > a_budgetBytes ? "One mesh exceeds stage budget" : "Resident exports leave insufficient stage budget";
			lastRequiredBytes = required;
			lastBudgetBytes = a_budgetBytes;
			return;
		}
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kTextureWidth;
		desc.Height = uint32_t(height);
		desc.MipLevels = desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_UINT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		a_transfer.texture = std::make_unique<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "RedstoneShadows::GeometryExport");
		a_transfer.sharedBytes = textureAllocation.SizeInBytes;
		a_transfer.geometry = std::make_shared<Geometry>();
		auto& geometry = *a_transfer.geometry;
		geometry.source = a_mesh;
		geometry.indexOffset = vertices.ByteWidth;
		geometry.validationOffset = vertices.ByteWidth + uint64_t(indices.ByteWidth / sizeof(uint16_t)) * sizeof(uint32_t);
		geometry.allocationBytes = bufferAllocation.SizeInBytes;
		DX::ThrowIfFailed(backend.d3d12Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(geometry.buffer.put())));
		geometry.buffer->SetName(L"RedstoneShadows::Geometry");
		auto view = [&](ID3D11Buffer* source, DXGI_FORMAT format, uint32_t count, winrt::com_ptr<ID3D11ShaderResourceView>& destination, const char* name) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = format;
			srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srv.Buffer.NumElements = count;
			DX::ThrowIfFailed(backend.d3d11Device->CreateShaderResourceView(source, &srv, destination.put()));
			Util::SetResourceName(destination.get(), name);
		};
		view(a_mesh->vertices.get(), DXGI_FORMAT_R32_UINT, vertices.ByteWidth / sizeof(uint32_t), a_transfer.vertices, "RedstoneShadows::ExportVertices SRV");
		view(a_mesh->indices.get(), DXGI_FORMAT_R16_UINT, indices.ByteWidth / sizeof(uint16_t), a_transfer.indices, "RedstoneShadows::ExportIndices SRV");
		const uint32_t constants[]{ a_mesh->stride / sizeof(uint32_t), a_mesh->vertexCount, 0, 0 };
		D3D11_BUFFER_DESC settingsDesc{};
		settingsDesc.ByteWidth = sizeof(constants);
		settingsDesc.Usage = D3D11_USAGE_IMMUTABLE;
		settingsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		const D3D11_SUBRESOURCE_DATA settingsData{ constants, 0, 0 };
		DX::ThrowIfFailed(backend.d3d11Device->CreateBuffer(&settingsDesc, &settingsData, a_transfer.settings.put()));
		Util::SetResourceName(a_transfer.settings.get(), "RedstoneShadows::GeometryExport CB");
		DX::ThrowIfFailed(backend.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(a_transfer.allocator.put())));
		DX::ThrowIfFailed(backend.d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a_transfer.allocator.get(), nullptr, IID_PPV_ARGS(a_transfer.commands.put())));
		D3D12_TEXTURE_COPY_LOCATION source{};
		source.pResource = a_transfer.texture->resource.get();
		source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_TEXTURE_COPY_LOCATION destination{};
		destination.pResource = geometry.buffer.get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		const auto textureDesc = source.pResource->GetDesc();
		uint64_t copyBytes = 0;
		backend.d3d12Device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &destination.PlacedFootprint, nullptr, nullptr, &copyBytes);
		if (destination.PlacedFootprint.Footprint.RowPitch != kTextureWidth * sizeof(uint32_t) || copyBytes > buffer.Width)
			throw std::runtime_error("Geometry export footprint mismatch");
		const auto read = CD3DX12_RESOURCE_BARRIER::Transition(source.pResource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
		a_transfer.commands->ResourceBarrier(1, &read);
		a_transfer.commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
		const D3D12_RESOURCE_BARRIER finish[]{
			CD3DX12_RESOURCE_BARRIER::Transition(source.pResource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
			CD3DX12_RESOURCE_BARRIER::Transition(geometry.buffer.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
		};
		a_transfer.commands->ResourceBarrier(uint32_t(std::size(finish)), finish);
		DX::ThrowIfFailed(a_transfer.commands->Close());
		winrt::com_ptr<ID3D11ComputeShader> previous;
		winrt::com_ptr<ID3D11ShaderResourceView> previousVertices, previousIndices;
		winrt::com_ptr<ID3D11UnorderedAccessView> previousOutput;
		winrt::com_ptr<ID3D11Buffer> previousSettings;
		backend.d3d11Context->CSGetShader(previous.put(), nullptr, nullptr);
		backend.d3d11Context->CSGetShaderResources(0, 1, previousVertices.put());
		backend.d3d11Context->CSGetShaderResources(1, 1, previousIndices.put());
		backend.d3d11Context->CSGetUnorderedAccessViews(0, 1, previousOutput.put());
		backend.d3d11Context->CSGetConstantBuffers(0, 1, previousSettings.put());
		ID3D11ShaderResourceView* inputs[]{ a_transfer.vertices.get(), a_transfer.indices.get() };
		backend.d3d11Context->CSSetShaderResources(0, uint32_t(std::size(inputs)), inputs);
		backend.d3d11Context->CSSetUnorderedAccessViews(0, 1, &a_transfer.texture->uav, nullptr);
		backend.d3d11Context->CSSetShader(exportShader.get(), nullptr, 0);
		auto* settings = a_transfer.settings.get();
		backend.d3d11Context->CSSetConstantBuffers(0, 1, &settings);
		a_transfer.submitted = true;
		const UINT clear[4]{};
		backend.d3d11Context->ClearUnorderedAccessViewUint(a_transfer.texture->uav, clear);
		backend.d3d11Context->Dispatch((kTextureWidth + 7) / 8, (uint32_t(height) + 7) / 8, 1);
		inputs[0] = previousVertices.get();
		inputs[1] = previousIndices.get();
		auto* output = previousOutput.get();
		backend.d3d11Context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
		backend.d3d11Context->CSSetShaderResources(0, uint32_t(std::size(inputs)), inputs);
		backend.d3d11Context->CSSetShader(previous.get(), nullptr, 0);
		settings = previousSettings.get();
		backend.d3d11Context->CSSetConstantBuffers(0, 1, &settings);
		const uint64_t produced = producer.Next();
		DX::ThrowIfFailed(backend.d3d11Context->Signal(producer.fence11.get(), produced));
		backend.d3d11Context->Flush();
		DX::ThrowIfFailed(backend.commandQueue->Wait(producer.fence12.get(), produced));
		ID3D12CommandList* lists[]{ a_transfer.commands.get() };
		backend.commandQueue->ExecuteCommandLists(1, lists);
		a_transfer.fence = ++nextFence;
		DX::ThrowIfFailed(backend.commandQueue->Signal(completion.get(), a_transfer.fence));
		a_transfer.generation = generation;
		++submitted;
	}

	void GeometryBridge::Advance(const Snapshot* a_snapshot, uint64_t a_budgetBytes)
	{
		if (!completion || failed) return;
		const uint64_t done = completion->GetCompletedValue();
		if (done == UINT64_MAX) { failed = true; failure = "Geometry export device removed"; return; }
		for (auto& transfer : transfers) {
			if (!transfer.submitted || !transfer.fence || transfer.fence > done) continue;
			if (transfer.generation == generation && currentMeshes.contains(transfer.geometry->source.get())) {
				const auto* key = transfer.geometry->source.get();
				geometries.emplace(key, std::move(transfer.geometry));
				++completed;
			}
			transfer = {};
		}
		if (!a_snapshot) return;
		if (snapshotGeneration != a_snapshot->generation || snapshotSweep != a_snapshot->sweep) {
			currentMeshes.clear();
			for (const auto& instance : a_snapshot->instances) currentMeshes.insert(instance.mesh.get());
			std::erase_if(geometries, [&](const auto& pair) { return !currentMeshes.contains(pair.first); });
			snapshotGeneration = a_snapshot->generation;
			snapshotSweep = a_snapshot->sweep;
		}
		Transfer* available = nullptr;
		for (auto& transfer : transfers) if (!transfer.geometry) { available = &transfer; break; }
		if (!available) { ++busyFrames; return; }
		for (size_t attempt = 0; attempt < a_snapshot->instances.size(); ++attempt) {
			const auto& instance = a_snapshot->instances[exportCursor++ % a_snapshot->instances.size()];
			const auto& mesh = instance.mesh;
			if (geometries.contains(mesh.get()) || std::ranges::any_of(transfers, [&](const Transfer& transfer) { return transfer.geometry && transfer.geometry->source == mesh; })) continue;
			try {
				Submit(*available, mesh, a_budgetBytes);
			} catch (const std::exception& error) {
				failed = true;
				failure = error.what();
				if (!available->submitted) *available = {};
			}
			break;
		}
	}

	json GeometryBridge::GetDiagnostics() const
	{
		return {
			{ "submittedExports", submitted }, { "completedExports", completed }, { "residentMeshes", geometries.size() },
			{ "residentBytes", ResidentBytes() }, { "busyFrames", busyFrames }, { "budgetDeferrals", budgetDeferrals },
			{ "failed", failed }, { "failure", failure }, { "cpuReadbacks", 0 },
			{ "lastDeferral", lastDeferral }, { "lastRequiredBytes", lastRequiredBytes }, { "lastBudgetBytes", lastBudgetBytes },
			{ "submittedFence", nextFence }, { "completedFence", completion ? completion->GetCompletedValue() : 0 },
			{ "coverageValid", false }
		};
	}

	std::shared_ptr<const Geometry> GeometryBridge::Find(const Mesh* a_mesh) const
	{
		const auto found = geometries.find(a_mesh);
		return found == geometries.end() ? nullptr : found->second;
	}
}

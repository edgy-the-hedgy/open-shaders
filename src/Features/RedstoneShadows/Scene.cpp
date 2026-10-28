#include "Scene.h"
#include "Globals.h"
#include "Utils/D3D.h"

namespace RedstoneShadow
{
	bool Scene::Initialize()
	{
		if (completion) return true;
		const D3D11_QUERY_DESC desc{ D3D11_QUERY_EVENT, 0 };
		if (FAILED(globals::d3d::device->CreateQuery(&desc, completion.put()))) { failure = "SCM copy query unavailable"; return false; }
		Util::SetResourceName(completion.get(), "RedstoneShadows::SCM CopyCompletion");
		return true;
	}
	void Scene::Reset()
	{
		const auto currentEpoch = CasterObservation::Epoch();
		if (published && published->scope.scene != currentEpoch) ++resetEpochDiscards;
		if (collecting && !discardPending && collecting->scope.scene != currentEpoch) ++resetEpochDiscards;
		published.reset();
		if (copiesPending) discardPending = true;
		else { collecting.reset(); meshes.clear(); failure.clear(); }
	}
	uint64_t Scene::ResidentBytes() const
	{
		uint64_t bytes = 0;
		for (const auto& cached : meshes) bytes += cached.mesh->bytes;
		return bytes;
	}
	std::shared_ptr<const Mesh> Scene::Acquire(const CasterObservation::Record& record, const SceneLimits& limits)
	{
		for (auto& cached : meshes) {
			if (cached.vertices.get() == record.vertices.get() && cached.indices.get() == record.indices.get() && cached.mesh->descriptor == record.descriptor) {
				cached.batch = collecting->sweep; ++reusedMeshes; return cached.mesh;
			}
		}
		D3D11_BUFFER_DESC vb{}, ib{};
		record.vertices->GetDesc(&vb); record.indices->GetDesc(&ib);
		constexpr uint64_t kAlignment = 64 * 1024;
		const auto reserve = [](uint64_t size) { return (size + kAlignment - 1) / kAlignment * kAlignment; };
		const auto bytes = reserve(vb.ByteWidth) + reserve(ib.ByteWidth);
		constexpr uint64_t kMaximumBatchCopyBytes = 16ull * 1024 * 1024;
		const auto copyBytes = uint64_t(vb.ByteWidth) + ib.ByteWidth;
		if (copyBytes > kMaximumBatchCopyBytes - batchCopiedBytes) return {};
		if (bytes > limits.bufferBytes || ResidentBytes() > limits.bufferBytes - bytes) return {};
		auto mesh = std::make_shared<Mesh>();
		mesh->stride = uint32_t(record.descriptor & 0xf) * sizeof(float);
		mesh->vertexCount = vb.ByteWidth / mesh->stride;
		mesh->descriptor = record.descriptor; mesh->bytes = bytes;
		auto create = [&](uint32_t width, winrt::com_ptr<ID3D11Buffer>& destination, const char* name) {
			D3D11_BUFFER_DESC desc{}; desc.ByteWidth = width; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&desc, nullptr, destination.put()));
			Util::SetResourceName(destination.get(), name);
		};
		create(vb.ByteWidth, mesh->vertices, "RedstoneShadows::SCM Vertices");
		create(ib.ByteWidth, mesh->indices, "RedstoneShadows::SCM Indices");
		meshes.push_back({ record.vertices, record.indices, mesh, collecting->sweep });
		globals::d3d::context->CopyResource(mesh->vertices.get(), record.vertices.get());
		globals::d3d::context->CopyResource(mesh->indices.get(), record.indices.get());
		copiedBytes += copyBytes;
		batchCopiedBytes += copyBytes;
		return mesh;
	}
	bool Scene::RetireCopies()
	{
		if (!copiesPending) return failure.empty();
		BOOL done = FALSE;
		const auto result = globals::d3d::context->GetData(completion.get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
		if (FAILED(result)) { failure = "SCM copy retirement failed"; return false; }
		if (result != S_OK || !done) { ++busyFrames; return false; }
		copiesPending = false;
		if (discardPending) { collecting.reset(); meshes.clear(); discardPending = false; failure.clear(); }
		else {
			std::erase_if(meshes, [&](const Cached& cached) { return cached.batch != collecting->sweep; });
			if (collecting->scope.scene == CasterObservation::Epoch()) published = std::move(collecting);
			else { ++copyEpochDiscards; collecting.reset(); }
		}
		return true;
	}
	void Scene::Advance(const SceneLimits& limits, uint64_t)
	{
		if (!completion || !RetireCopies() || published) return;
		auto* batch = CasterObservation::Acquire();
		if (!batch) return;
		const SKSE::stl::scope_exit release([&]() noexcept { CasterObservation::Release(batch); });
		if (batch->scope.scene != CasterObservation::Epoch()) { ++queuedEpochDiscards; return; }
		try {
			batchCopiedBytes = 0;
			collecting = std::make_shared<Snapshot>(); collecting->scope = batch->scope;
			collecting->generation = batch->scope.scene; collecting->sweep = batch->scope.batch;
			collecting->firstFrame = collecting->lastFrame = batch->scope.frame;
			collecting->truncated = batch->incomplete || batch->overflow;
			collecting->omissions["nativeObservationOmissions"] = batch->omissions;
			collecting->instances.reserve(std::min(batch->count, limits.maxInstances));
			for (uint32_t i = 0; i < batch->count; ++i) {
				if (collecting->instances.size() >= limits.maxInstances) { collecting->truncated = true; break; }
				const auto& record = batch->records[i]; auto mesh = Acquire(record, limits);
				if (!mesh) { collecting->truncated = true; ++collecting->omissions["bufferBudget"]; continue; }
				Instance instance; instance.identity = uint64_t(i) + 1; instance.mesh = std::move(mesh);
				instance.world = record.world; instance.bound = record.bound; instance.material.twoSided = record.twoSided;
				instance.firstIndex = record.first; instance.indexCount = record.count; instance.observationFrame = batch->scope.frame;
				collecting->instances.push_back(std::move(instance));
			}
			globals::d3d::context->End(completion.get()); copiesPending = true;
		} catch (const std::exception& error) {
			if (collecting) { globals::d3d::context->End(completion.get()); copiesPending = discardPending = true; }
			failure = error.what(); published.reset();
		}
	}
	json Scene::GetDiagnostics() const
	{
		const auto* snapshot = published ? published.get() : collecting.get();
		return {
			{ "scope", "observed SCM local-light subset; not directional coverage" }, { "independentTopologyWalk", false },
			{ "sourceFrame", snapshot ? snapshot->scope.frame : 0 }, { "sceneGeneration", snapshot ? snapshot->scope.scene : 0 },
			{ "lightGeneration", snapshot ? snapshot->scope.light : 0 }, { "slot", snapshot ? snapshot->scope.slot : 0 },
			{ "batch", snapshot ? snapshot->scope.batch : 0 }, { "incomplete", snapshot && snapshot->truncated },
			{ "instances", snapshot ? snapshot->instances.size() : 0 }, { "copiedBytes", copiedBytes }, { "meshReuses", reusedMeshes },
			{ "residentBytes", ResidentBytes() }, { "busyFrames", busyFrames }, { "failure", failure },
			{ "copyPending", copiesPending }, { "coverageValid", false },
			{ "queuedEpochDiscards", queuedEpochDiscards }, { "copyEpochDiscards", copyEpochDiscards }, { "resetEpochDiscards", resetEpochDiscards }
		};
	}
}

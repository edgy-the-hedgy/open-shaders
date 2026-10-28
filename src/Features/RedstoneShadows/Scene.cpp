#include "Scene.h"

#include "Globals.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include <bit>
#include <chrono>

namespace RedstoneShadow
{
	bool Scene::Initialize()
	{
		if (completion) return true;
		const D3D11_QUERY_DESC desc{ D3D11_QUERY_EVENT, 0 };
		const auto result = globals::d3d::device->CreateQuery(&desc, completion.put());
		if (FAILED(result)) {
			failure = std::format("Scene copy query failed: {:#x}", uint32_t(result));
			return false;
		}
		Util::SetResourceName(completion.get(), "RedstoneShadows::SceneCopyCompletion");
		return true;
	}

	void Scene::Reset()
	{
		++generation;
		pending.clear();
		visited.clear();
		identities.clear();
		published.reset();
		if (copiesPending) discardPending = true;
		else {
			collecting.reset();
			meshes.clear();
		}
		residentBytes = ResidentBytes();
		failure.clear();
	}

	void Scene::Enqueue(RE::NiAVObject* a_object)
	{
		if (!a_object || visited.contains(a_object)) return;
		if (visited.size() >= kMaximumNodes) {
			collecting->truncated = true;
			return;
		}
		RE::NiPointer<RE::NiAVObject> owned(a_object);
		visited.emplace(a_object, owned);
		pending.push_back({ std::move(owned), 0 });
		++collecting->visitedNodes;
	}

	void Scene::Begin(uint64_t a_frame)
	{
		collecting = std::make_unique<Snapshot>();
		collecting->generation = generation;
		collecting->sweep = ++sweep;
		collecting->firstFrame = a_frame;
		visited.clear();
		auto addCell = [&](RE::TESObjectCELL* cell) {
			if (!cell || !cell->IsAttached()) return;
			const auto& data = cell->GetRuntimeData();
			if (data.loadedData) Enqueue(data.loadedData->cell3D.get());
			if (const auto* land = data.cellLand; land && land->loadedData)
				for (auto* mesh : land->loadedData->mesh) Enqueue(mesh);
		};
		if (auto* player = globals::game::player) {
			addCell(player->GetParentCell());
			Enqueue(player->Get3D(false));
		}
		const auto* tes = globals::game::tes;
		const auto* grid = tes ? tes->gridCells : nullptr;
		if (grid && grid->cells) {
			const auto length = std::min(grid->length, kMaximumGridLength);
			collecting->truncated |= length != grid->length;
			for (uint32_t x = 0; x < length; ++x)
				for (uint32_t y = 0; y < length; ++y) addCell(grid->GetCell(x, y));
		}
		Enqueue(RE::BGSTerrainManager::GetLODLandRoot().get());
		if (pending.empty()) ++collecting->omissions["noLoadedRoots"];
	}

	uint64_t Scene::ResidentBytes() const
	{
		std::unordered_set<const Mesh*> counted;
		uint64_t bytes = 0;
		auto count = [&](const std::shared_ptr<const Mesh>& mesh) {
			if (mesh && counted.insert(mesh.get()).second) bytes += mesh->bytes;
		};
		for (const auto& [key, cached] : meshes) count(cached.owned);
		if (collecting) for (const auto& instance : collecting->instances) count(instance.mesh);
		if (published) for (const auto& instance : published->instances) count(instance.mesh);
		return bytes;
	}

	std::shared_ptr<const Mesh> Scene::AcquireMesh(RE::BSGeometry* a_geometry, const SceneLimits& a_limits, uint64_t a_frame)
	{
		const auto& data = a_geometry->GetGeometryRuntimeData();
		auto* source = static_cast<RE::BSGraphics::TriShape*>(data.rendererData);
		if (!source || !source->vertexBuffer || !source->indexBuffer) {
			++collecting->omissions["missingBuffers"];
			return {};
		}
		winrt::com_ptr<ID3D11Buffer> vertices, indices;
		vertices.copy_from(Util::AsReal(source->vertexBuffer));
		indices.copy_from(Util::AsReal(source->indexBuffer));
		const auto descriptor = std::bit_cast<uint64_t>(data.vertexDesc);
		D3D11_BUFFER_DESC vertexDesc{}, indexDesc{};
		vertices->GetDesc(&vertexDesc);
		indices->GetDesc(&indexDesc);
		const bool immutable = vertexDesc.Usage == D3D11_USAGE_IMMUTABLE && indexDesc.Usage == D3D11_USAGE_IMMUTABLE;
		const MeshKey key{ reinterpret_cast<uintptr_t>(vertices.get()), reinterpret_cast<uintptr_t>(indices.get()), descriptor, immutable ? 0 : a_frame };
		if (auto cached = meshes.find(key); cached != meshes.end()) {
			cached->second.sweep = sweep;
			++reusedMeshes;
			return cached->second.owned;
		}
		const auto stride = uint32_t(descriptor & 0xf) * sizeof(float);
		if (!data.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) ||
			!data.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC) || stride < sizeof(float) * 3 ||
			vertexDesc.ByteWidth % stride || indexDesc.ByteWidth % sizeof(uint16_t)) {
			++collecting->omissions["unsupportedVertexLayout"];
			return {};
		}
		const uint64_t bytes = uint64_t(vertexDesc.ByteWidth) + indexDesc.ByteWidth;
		constexpr uint64_t kBufferReservationAlignment = 64 * 1024;
		auto reserve = [](uint64_t size) { return (size + kBufferReservationAlignment - 1) / kBufferReservationAlignment * kBufferReservationAlignment; };
		const auto reservation = reserve(vertexDesc.ByteWidth) + reserve(indexDesc.ByteWidth);
		if (bytes > kMaximumCopyBytesPerFrame) {
			++collecting->omissions["meshExceedsFrameCopyBudget"];
			return {};
		}
		if (reservation > a_limits.bufferBytes || residentBytes > a_limits.bufferBytes - reservation) {
			++collecting->omissions["bufferBudget"];
			return {};
		}
		if (frameCopiedBytes > kMaximumCopyBytesPerFrame - bytes) {
			retryObservation = true;
			return {};
		}
		auto owned = std::make_shared<Mesh>();
		owned->stride = uint32_t(stride);
		owned->vertexCount = vertexDesc.ByteWidth / owned->stride;
		owned->descriptor = descriptor;
		owned->bytes = reservation;
		auto create = [&](uint32_t size, winrt::com_ptr<ID3D11Buffer>& destination, const char* name) {
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = size;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&desc, nullptr, destination.put()));
			Util::SetResourceName(destination.get(), name);
		};
		create(vertexDesc.ByteWidth, owned->vertices, "RedstoneShadows::OwnedVertices");
		create(indexDesc.ByteWidth, owned->indices, "RedstoneShadows::OwnedIndices");
		const auto cached = meshes.emplace(key, CachedMesh{ std::move(vertices), std::move(indices), owned, sweep }).first;
		globals::d3d::context->CopyResource(owned->vertices.get(), cached->second.sourceVertices.get());
		globals::d3d::context->CopyResource(owned->indices.get(), cached->second.sourceIndices.get());
		copiedBytes += bytes;
		frameCopiedBytes += bytes;
		residentBytes += reservation;
		return owned;
	}

	void Scene::Observe(RE::BSGeometry* a_geometry, const SceneLimits& a_limits, uint64_t a_frame)
	{
		auto& data = a_geometry->GetGeometryRuntimeData();
		auto* property = data.shaderProperty.get();
		if (!property || !property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows)) return;
		const auto type = a_geometry->GetType();
		if (data.skinInstance) { ++collecting->omissions["skinnedDeformation"]; return; }
		if (property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim)) { ++collecting->omissions["vegetationDeformation"]; return; }
		if (property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kLODLandscape)) { ++collecting->omissions["lodLandscapeDeformation"]; return; }
		if (type != RE::BSGeometry::Type::kTriShape && type != RE::BSGeometry::Type::kSubIndexTriShape &&
			type != RE::BSGeometry::Type::kMultiIndexTriShape) {
			++collecting->omissions["unsupportedGeometryType"];
			return;
		}
		auto* shape = a_geometry->AsTriShape();
		if (!shape || !property->material || property->material->GetType() != RE::BSShaderMaterial::Type::kLighting ||
			!property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kZBufferWrite) ||
			property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kRefraction, RE::BSShaderProperty::EShaderPropertyFlag::kTempRefraction) ||
			(data.alphaProperty && data.alphaProperty->GetAlphaBlending())) {
			++collecting->omissions["unsupportedMaterial"];
			return;
		}
		if (collecting->instances.size() >= a_limits.maxInstances ||
			(!identities.contains(a_geometry) && identities.size() >= a_limits.maxInstances)) {
			++collecting->omissions["instanceBudget"];
			return;
		}
		Instance instance;
		const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
		instance.material.uvTransform = { material->texCoordScale[0].x, material->texCoordScale[0].y, material->texCoordOffset[0].x, material->texCoordOffset[0].y };
		instance.material.alpha = material->materialAlpha;
		instance.material.textureClampMode = material->textureClampMode;
		if (data.alphaProperty) instance.material.alphaFlags = data.alphaProperty->alphaFlags;
		instance.material.twoSided = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
		if (data.alphaProperty && data.alphaProperty->GetAlphaTesting()) {
			instance.material.alphaTest = true;
			instance.material.alphaCutoff = data.alphaProperty->alphaThreshold / 255.0f;
			auto* texture = property->GetBaseTexture();
			if (!data.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_UV) || !texture || !texture->rendererTexture || !texture->rendererTexture->resourceView) {
				++collecting->omissions["missingAlphaTexture"];
				return;
			}
			instance.material.alphaTexture.copy_from(Util::AsReal(texture->rendererTexture->resourceView));
		}
		alignas(16) DirectX::XMFLOAT3X4 world;
		Util::StoreTransform3x4NoScale(world, a_geometry->world);
		bool finite = std::isfinite(a_geometry->world.scale) && a_geometry->world.scale != 0;
		for (auto& row : world.m) {
			for (uint32_t i = 0; i < 3; ++i) row[i] *= a_geometry->world.scale;
			for (float value : row) finite &= std::isfinite(value);
		}
		instance.world = world;
		const auto& bound = a_geometry->worldBound;
		instance.bound = { bound.center.x, bound.center.y, bound.center.z, bound.radius };
		for (float value : { bound.center.x, bound.center.y, bound.center.z, bound.radius, instance.material.alpha,
			instance.material.uvTransform.x, instance.material.uvTransform.y, instance.material.uvTransform.z, instance.material.uvTransform.w }) finite &= std::isfinite(value);
		if (!finite || bound.radius < 0) { ++collecting->omissions["invalidTransformOrMaterial"]; return; }
		instance.mesh = AcquireMesh(a_geometry, a_limits, a_frame);
		if (!instance.mesh) return;
		auto [identity, inserted] = identities.try_emplace(a_geometry);
		if (inserted) {
			identity->second.object.reset(a_geometry);
			identity->second.serial = nextIdentity++;
		}
		identity->second.sweep = sweep;
		instance.identity = identity->second.serial;
		instance.observationFrame = a_frame;
		instance.indexCount = uint32_t(shape->GetTrishapeRuntimeData().triangleCount) * 3;
		auto append = [&](uint32_t first, uint32_t count) {
			D3D11_BUFFER_DESC desc{};
			instance.mesh->indices->GetDesc(&desc);
			if ((uint64_t(first) + count) * sizeof(uint16_t) > desc.ByteWidth || count % 3) {
				++collecting->omissions["invalidIndexRange"];
				return;
			}
			if (!count) return;
			if (collecting->instances.size() >= a_limits.maxInstances) { ++collecting->omissions["instanceBudget"]; return; }
			instance.firstIndex = first;
			instance.indexCount = count;
			collecting->instances.push_back(instance);
		};
		if (auto* segmented = a_geometry->AsSubIndexTriShape()) {
			const auto& segments = segmented->GetSubIndexedTrishapeRuntimeData();
			if (segments.nonSegmented) append(0, instance.indexCount);
			else if (!segments.segmentData || segments.numSegments > a_limits.maxInstances) ++collecting->omissions["unsupportedTerrainSegments"];
			else for (uint32_t i = 0; i < segments.numSegments; ++i) {
				const auto& range = segments.segmentData[i];
				if (range.flags) append(range.index, uint32_t(range.numTris) * 3);
			}
		} else append(0, instance.indexCount);
	}

	void Scene::Complete()
	{
		copiesPending = false;
		if (discardPending) {
			collecting.reset();
			meshes.clear();
			residentBytes = 0;
			discardPending = false;
			return;
		}
		std::erase_if(identities, [&](const auto& entry) { return entry.second.sweep != sweep; });
		std::erase_if(meshes, [&](const auto& pair) {
			const auto& entry = pair.second;
			D3D11_BUFFER_DESC vertices{}, indices{};
			entry.sourceVertices->GetDesc(&vertices);
			entry.sourceIndices->GetDesc(&indices);
			return entry.sweep != sweep || vertices.Usage != D3D11_USAGE_IMMUTABLE || indices.Usage != D3D11_USAGE_IMMUTABLE;
		});
		published = std::move(collecting);
		residentBytes = ResidentBytes();
	}

	bool Scene::RetireCopies()
	{
		if (!failure.empty()) return false;
		if (!copiesPending) return true;
		BOOL done = FALSE;
		const auto result = globals::d3d::context->GetData(completion.get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
		if (FAILED(result)) {
			failure = std::format("Scene copy completion failed: {:#x}", uint32_t(result));
			return false;
		}
		if (result != S_OK || !done) { ++busyFrames; return false; }
		copiesPending = false;
		if (discardPending || pending.empty()) Complete();
		return true;
	}

	void Scene::Advance(const SceneLimits& a_limits, uint64_t a_frame)
	{
		if (!completion || !RetireCopies()) return;
		const auto started = std::chrono::steady_clock::now();
		try {
			frameCopiedBytes = 0;
			retryObservation = false;
			if (!collecting) Begin(a_frame);
			for (uint32_t count = 0; count < a_limits.nodesPerFrame && !pending.empty(); ++count) {
				auto& entry = pending.back();
				if (auto* node = entry.object->AsNode()) {
					auto& children = node->GetChildren();
					if (entry.child >= kMaximumNodes && entry.child < children.size()) {
						collecting->truncated = true;
						pending.pop_back();
					} else if (entry.child < children.size()) Enqueue(children[entry.child++].get());
					else pending.pop_back();
				} else {
					auto object = std::move(entry.object);
					pending.pop_back();
					if (auto* geometry = object->AsGeometry()) Observe(geometry, a_limits, a_frame);
					if (retryObservation) {
						pending.push_back({ std::move(object), 0 });
						break;
					}
				}
			}
			collecting->lastFrame = a_frame;
			globals::d3d::context->End(completion.get());
			copiesPending = true;
		} catch (const std::exception& error) {
			Reset();
			failure = error.what();
		}
		acquisitionMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	}

	json Scene::GetDiagnostics() const
	{
		json result{
			{ "generation", generation }, { "sweep", sweep }, { "pendingNodes", pending.size() },
			{ "residentBufferBytes", ResidentBytes() }, { "copiedBufferBytes", copiedBytes }, { "meshReuses", reusedMeshes },
			{ "maximumCopyBytesPerFrame", kMaximumCopyBytesPerFrame }, { "lastBatchCopyBytes", frameCopiedBytes },
			{ "copyCompletionPending", copiesPending }, { "copyBusyFrames", busyFrames }, { "lastAcquisitionCPUms", acquisitionMs },
			{ "failure", failure }, { "published", bool(published) }, { "productionCoverageValid", false },
			{ "coverageLimit", "Sweeps span frames; skins, vegetation deformation, instancing and LOD layouts require additional producers. Hidden-node versus camera-cull policy is unresolved. Alpha textures retain views, not immutable texel snapshots." }
		};
		const auto* snapshot = published ? published.get() : collecting.get();
		if (snapshot) {
			result["snapshot"] = {
				{ "generation", snapshot->generation }, { "sweep", snapshot->sweep },
				{ "firstFrame", snapshot->firstFrame }, { "lastFrame", snapshot->lastFrame },
				{ "instances", snapshot->instances.size() }, { "visitedNodes", snapshot->visitedNodes },
				{ "truncated", snapshot->truncated }, { "omissions", snapshot->omissions }
			};
		}
		return result;
	}
}

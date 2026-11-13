#include "GrassBucketStore.h"

#include <numbers>
#include <thread>

GrassBucketStore::~GrassBucketStore()
{
	decltype(shapes) retiredShapes;
	{
		std::scoped_lock lock(pendingMutex);
		stopping = true;

		for (const auto& [native, record] : shapes)
			record->registered.store(false, std::memory_order_release);

		retiredShapes.swap(shapes);
	}

	preparation.reset();
	allocator.reset();
	publishedSnapshot.store(nullptr, std::memory_order_release);
	cullSnapshot.reset();
	retiredSnapshot.reset();
	retiredProxies.clear();
	buckets.clear();
	activeShapes.clear();
	readyEvents.clear();
	pendingEvents.clear();
}

void GrassBucketStore::RegisterModel(RE::BSMultiStreamInstanceTriShape* shape, const char* modelPath)
{
	if (!shape || !modelPath || !*modelPath)
		return;

	auto& mesh = meshLibrary.RegisterMesh(modelPath);
	if (auto previousRegistration = FindShape(shape); previousRegistration && previousRegistration->mesh == &mesh)
		return;

	auto registration = std::make_shared<GrassShape>();
	registration->native = shape;
	registration->mesh = &mesh;
	registration->geometry.reset(static_cast<RE::BSMultiStreamInstanceTriShape*>(shape->Clone()));

	if (!registration->geometry)
		return;

	std::shared_ptr<GrassShape> previousRegistration;
	{
		std::scoped_lock lock(pendingMutex);
		if (stopping)
			return;

		auto& entry = shapes[shape];
		if (entry && entry->mesh == &mesh)
			return;

		previousRegistration = std::exchange(entry, registration);
		if (previousRegistration) {
			previousRegistration->registered.store(false, std::memory_order_release);
			QueueEvent(Removal{ previousRegistration });
		}
	}

	std::vector<uint32_t> groupIndices;
	{
		EngineGroupLock lock;
		const auto& instances = shape->GetMultiStreamTrishapeRuntimeData().instanceGroups;

		for (uint32_t i = 0; i < instances.size(); ++i) {
			if (instances[i])
				groupIndices.push_back(i);
		}
	}

	for (uint32_t group : groupIndices)
		CaptureEngineGroup(shape, group);
}

void GrassBucketStore::CaptureEngineGroup(RE::BSMultiStreamInstanceTriShape* shape, uint32_t engineGroup)
{
	if (!shape || engineGroup == UINT32_MAX)
		return;

	std::scoped_lock lifecycleLock(pendingMutex);
	const auto entry = shapes.find(shape);
	if (stopping || entry == shapes.end())
		return;

	EngineGroupLock engineLock;
	CaptureEngineGroup(entry->second, engineGroup);
}

void GrassBucketStore::CaptureQueuedGroups()
{
	std::scoped_lock lifecycleLock(pendingMutex);
	if (stopping)
		return;

	EngineGroupLock engineLock;
	for (const auto& [native, registration] : shapes) {
		const auto& data = native->GetMultiStreamTrishapeRuntimeData();
		if (registration->groups.size() == data.activeGroupCount)
			continue;

		for (uint32_t index = 0; index < data.instanceGroups.size(); ++index) {
			if (data.instanceGroups[index] && !registration->groups.contains(index))
				CaptureEngineGroup(registration, index);
		}
	}
}

void GrassBucketStore::CaptureEngineGroup(const std::shared_ptr<GrassShape>& registration, uint32_t engineGroup)
{
	auto* shape = registration->native;
	auto property = shape->GetGeometryRuntimeData().shaderProperty;
	if (!property || property->GetRTTI() != globals::rtti::BSGrassShaderPropertyRTTI.get())
		return;

	const auto& groups = shape->GetMultiStreamTrishapeRuntimeData().instanceGroups;
	const auto* group = engineGroup < groups.size() ? groups[engineGroup] : nullptr;
	const auto* vertexBuffer = group ? group->vertexBuffer : nullptr;
	if (!vertexBuffer || !vertexBuffer->m_data || !group->instanceCount)
		return;

	// The engine initializes only the low 32 bits of the vertex buffer's byte width.
	const uint32_t byteWidth = (uint32_t)vertexBuffer->byteWidth;
	auto capture = CaptureMetadata(shape, group->instanceCount, byteWidth / group->instanceCount);
	if (!capture)
		return;

	capture->shape = registration;
	capture->slice.owner = registration.get();
	capture->slice.engineGroup = engineGroup;
	capture->asset.key.mesh = registration->mesh;
	const auto* data = static_cast<const uint8_t*>(vertexBuffer->m_data);
	capture->sourceData.assign(data, data + (size_t)group->instanceCount * kGrassStride);

	QueueEvent(std::move(*capture));
}

void GrassBucketStore::RemoveEngineGroup(RE::BSMultiStreamInstanceTriShape* shape, uint32_t engineGroup,
	void (*remove)(RE::BSMultiStreamInstanceTriShape*, uint32_t))
{
	std::scoped_lock lock(pendingMutex);
	remove(shape, engineGroup);

	if (engineGroup == UINT32_MAX)
		return;

	if (auto entry = shapes.find(shape); entry != shapes.end())
		QueueEvent(Removal{ entry->second, engineGroup });
}

void GrassBucketStore::StageRemoval(RE::BSMultiStreamInstanceTriShape* shape)
{
<<<<<<< HEAD
	std::scoped_lock lk(pendingMutex);
	// ApplyPending runs removals before captures, so a capture left queued here would re-add the destroyed shape and its freed texture.
	std::erase_if(pendingCaptures, [shape](const PendingCapture& pc) { return pc.shape == shape; });
	pendingRemoves.push_back(shape);
||||||| 12b261724
	std::scoped_lock lk(pendingMutex);
	pendingRemoves.push_back(shape);
=======
	decltype(shapes)::node_type retired;
	{
		std::scoped_lock lock(pendingMutex);
		retired = shapes.extract(shape);

		if (retired) {
			retired.mapped()->registered.store(false, std::memory_order_release);
			QueueEvent(Removal{ retired.mapped() });
		}
	}
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
}

GrassBucketStore::EngineGroupLock::EngineGroupLock()
{
	while (_InterlockedCompareExchange(word.get(), 1, 0))
		std::this_thread::yield();
}

<<<<<<< HEAD
bool GrassBucketStore::HasPending()
{
	std::scoped_lock lk(pendingMutex);
	return !pendingCaptures.empty() || !pendingRemoves.empty();
}

void GrassBucketStore::DiscardPending()
||||||| 12b261724
void GrassBucketStore::DiscardPending()
=======
GrassBucketStore::EngineGroupLock::~EngineGroupLock()
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
{
	_InterlockedExchange(word.get(), 0);
}

std::optional<PendingCapture> GrassBucketStore::CaptureMetadata(RE::BSMultiStreamInstanceTriShape* shape, uint32_t count, uint32_t stride)
{
	auto& geometry = shape->GetGeometryRuntimeData();
	auto* grassProperty = static_cast<RE::BSGrassShaderProperty*>(geometry.shaderProperty.get());
	auto* texture = grassProperty->GetBaseTexture();
	auto* material = grassProperty->material;
	auto* rendererData = geometry.rendererData;

	if (!count || stride != kGrassStride || !texture || !material || material->GetType() != RE::BSShaderMaterial::Type::kLighting ||
		!rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
		return std::nullopt;

	const auto& bound = shape->GetModelData().modelBound;

	PendingCapture capture;
	auto& asset = capture.asset;
	auto& draw = asset.key.draw;
	asset.vertexBuffer.copy_from(reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer));
	asset.indexBuffer.copy_from(reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer));

	asset.material.reset(material);
	// TruePBR marks converted grass properties with kMenuScreen.
	draw.pbrMaterial = grassProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMenuScreen);
	draw.sphereNormal = grassProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kEffectLighting);
	asset.key.material = GrassMeshLibrary::CaptureMaterial(material, draw.pbrMaterial);
	draw.uniformScale = grassProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kUniformScale);
	asset.key.descVal = *reinterpret_cast<const uint64_t*>(&geometry.vertexDesc);

	asset.diffuseTexture = RE::NiPointer<RE::NiSourceTexture>(texture);
	capture.slice.count = count;

	asset.key.triCount = shape->GetTrishapeRuntimeData().triangleCount;
	draw.boundCenter = { bound.center.x, bound.center.y, bound.center.z };
	draw.modelRadius = bound.radius;
	draw.wavePeriod = grassProperty->wavePeriod;

	capture.slice.origin = shape->world.translate;

	// The wind blows along the model's Y axis.
	RE::NiPoint3 windDirection{ shape->world.rotate.entry[1][0], shape->world.rotate.entry[1][1], shape->world.rotate.entry[1][2] };
	windDirection.Unitize();
	draw.windDirection[0] = windDirection.x;
	draw.windDirection[1] = windDirection.y;

	return capture;
}

// The engine stores position and brightness in halves 0-3, rotation rows at (4,5,6), (8,9,10), (12,7,11), and size variance at 13.
// Pack the orthonormal rotation as a quaternion with its largest component omitted; Common/GrassInstance.hlsli decodes it.
void GrassBucketStore::PackInstances(GrassChunk& slice, const void* sourceData)
{
	using DirectX::PackedVector::XMConvertHalfToFloat;
	slice.data.resize((size_t)slice.count * kSourceStride);
	slice.maxScale = 1.0f;
	RE::NiPoint3 localMin{ FLT_MAX, FLT_MAX, FLT_MAX };
	RE::NiPoint3 localMax{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

	const auto* sourceBytes = static_cast<const uint8_t*>(sourceData);
	const auto packComponent = [](float component) {
		const float normalized = std::clamp(component * std::numbers::sqrt2_v<float>, -1.0f, 1.0f);
		return (uint32_t)(std::lround(normalized * 16383.0f) + 16384);
	};

	for (uint32_t instanceIndex = 0; instanceIndex < slice.count; ++instanceIndex) {
		uint16_t halfValues[16];
		std::memcpy(halfValues, sourceBytes + (size_t)instanceIndex * kGrassStride, sizeof(halfValues));
		float values[16];

		for (int componentIndex = 0; componentIndex < 13; ++componentIndex)
			values[componentIndex] = XMConvertHalfToFloat(halfValues[componentIndex]);

		slice.maxScale = std::max(slice.maxScale, std::abs(1.0f + XMConvertHalfToFloat(halfValues[13])));
		localMin.x = std::min(localMin.x, values[0]);
		localMin.y = std::min(localMin.y, values[1]);
		localMin.z = std::min(localMin.z, values[2]);
		localMax.x = std::max(localMax.x, values[0]);
		localMax.y = std::max(localMax.y, values[1]);
		localMax.z = std::max(localMax.z, values[2]);

		const DirectX::XMFLOAT3X3 rotation{ values[4], values[5], values[6], values[8], values[9], values[10], values[12], values[7], values[11] };
		DirectX::XMFLOAT4 quaternion;
		// Engine rotation rows act on column vectors, while DirectXMath uses row vectors.
		DirectX::XMStoreFloat4(&quaternion, DirectX::XMQuaternionRotationMatrix(DirectX::XMMatrixTranspose(DirectX::XMLoadFloat3x3(&rotation))));
		const float components[]{ quaternion.x, quaternion.y, quaternion.z, quaternion.w };
		uint32_t omittedComponent = 0;
		for (uint32_t componentIndex = 1; componentIndex < 4; ++componentIndex) {
			if (std::abs(components[componentIndex]) > std::abs(components[omittedComponent]))
				omittedComponent = componentIndex;
		}

		const float rotationSign = components[omittedComponent] < 0.0f ? -1.0f : 1.0f;
		const float rotationLength = std::sqrt(components[0] * components[0] + components[1] * components[1] + components[2] * components[2] + components[3] * components[3]);
		uint32_t packedComponents[3];

		for (uint32_t componentIndex = 0, packedIndex = 0; componentIndex < 4; ++componentIndex) {
			if (componentIndex != omittedComponent)
				packedComponents[packedIndex++] = packComponent(rotationSign * components[componentIndex] / rotationLength);
		}

		const uint32_t brightness = (uint32_t)std::lround(std::clamp(values[3], 0.0f, 1.0f) * 65535.0f);
		const uint32_t packed[4] = {
			(uint32_t)halfValues[0] | ((uint32_t)halfValues[1] << 16),
			(uint32_t)halfValues[2] | (brightness << 16),
			packedComponents[0] | (packedComponents[1] << 15) | (omittedComponent << 30),
			packedComponents[2] | ((uint32_t)halfValues[13] << 16),
		};

		std::memcpy(slice.data.data() + (size_t)instanceIndex * kSourceStride, packed, sizeof(packed));
	}

	slice.bounds.lo[0] = slice.origin.x + localMin.x;
	slice.bounds.lo[1] = slice.origin.y + localMin.y;
	slice.bounds.lo[2] = slice.origin.z + localMin.z;
	slice.bounds.hi[0] = slice.origin.x + localMax.x;
	slice.bounds.hi[1] = slice.origin.y + localMax.y;
	slice.bounds.hi[2] = slice.origin.z + localMax.z;
}

void GrassBucketStore::QueueEvent(PendingEvent event)
{
	if (stopping)
		return;

	std::visit([](auto& command) { command.shape->pendingChanges.fetch_add(1, std::memory_order_release); }, event);
	if (auto* removal = std::get_if<Removal>(&event)) {
		auto& groups = removal->shape->groups;

		if (removal->engineGroup == UINT32_MAX) {
			for (auto& [index, group] : groups)
				group->current.store(false, std::memory_order_release);
			groups.clear();
		} else if (auto group = groups.extract(removal->engineGroup)) {
			removal->lifetime = std::move(group.mapped());
			removal->lifetime->current.store(false, std::memory_order_release);
		}

		readyEvents.push_back(std::move(event));
		return;
	}

	auto& capture = std::get<PendingCapture>(event);
	auto& group = capture.shape->groups[capture.slice.engineGroup];
	if (group) {
		group->current.store(false, std::memory_order_release);
		capture.shape->pendingChanges.fetch_add(1, std::memory_order_release);
		readyEvents.emplace_back(Removal{ capture.shape, capture.slice.engineGroup, group });
	}

	capture.slice.lifetime = group = std::make_shared<GrassGroup>();
	pendingEvents.push_back(std::move(event));

	if (preparing)
		return;

	preparing = true;

	if (!preparation)
		preparation = std::make_unique<BS::thread_pool<>>(1);

	preparation->detach_task([this] { PreparePending(); });
}

void GrassBucketStore::PreparePending()
{
	std::vector<PendingEvent> events;
	for (;;) {
		{
			std::scoped_lock lock(pendingMutex);
			if (pendingEvents.empty()) {
				preparing = false;
				return;
			}
			events.swap(pendingEvents);
		}

		for (auto& event : events) {
			if (auto* capture = std::get_if<PendingCapture>(&event)) {
				if (capture->shape->registered.load(std::memory_order_acquire) && capture->slice.lifetime->current.load(std::memory_order_acquire))
					PackInstances(capture->slice, capture->sourceData.data());
				capture->sourceData = std::vector<uint8_t>{};
			}
		}

		{
			std::scoped_lock lock(pendingMutex);
			readyEvents.insert(readyEvents.end(), std::make_move_iterator(events.begin()), std::make_move_iterator(events.end()));
		}
		events.clear();
	}
}

void GrassBucketStore::EndFrame()
{
	retiredProxies.clear();
	DrainPending();

	if (buckets.empty())
		compactionScratch.reset();

	PublishCullSnapshot();
}

void GrassBucketStore::DrainPending()
{
	std::vector<PendingEvent> events;
	{
		std::scoped_lock lock(pendingMutex);
		events.swap(readyEvents);
	}

	if (events.empty())
		return;

	std::unordered_set<GrassBucket*> affected;
	std::vector<std::unique_ptr<GrassChunk>> retired;

	for (auto& event : events) {
		if (auto* capture = std::get_if<PendingCapture>(&event))
			ApplyCapture(*capture, affected, retired);
		else
			ApplyRemoval(std::get<Removal>(event), affected, retired);
	}

	for (auto* bucket : affected) {
		PruneSlices(*bucket);

		if (bucket->slices.empty())
			EraseBucket(*bucket);
	}

	std::erase_if(activeShapes, [](const auto& shape) {
		if (!shape->chunks.empty())
			return false;

		shape->admitted = false;
		shape->current = nullptr;

		return true;
	});

	RetireSlices(retired);
}

void GrassBucketStore::ApplyCapture(PendingCapture& capture, std::unordered_set<GrassBucket*>& affected, std::vector<std::unique_ptr<GrassChunk>>& retired)
{
	auto& shape = *capture.shape;
	shape.pendingChanges.fetch_sub(1, std::memory_order_release);
	if (!shape.registered.load(std::memory_order_acquire) || !capture.slice.lifetime->current.load(std::memory_order_acquire)) {
		if (!capture.slice.data.empty())
			retired.push_back(std::make_unique<GrassChunk>(std::move(capture.slice)));
		return;
	}

	auto [bucketEntry, inserted] = buckets.try_emplace(capture.asset.key);
	auto& bucket = bucketEntry->second;

	if (inserted) {
		bucket.asset = std::move(capture.asset);
		if (!meshLibrary.CreateAsset(bucket.asset, capture.shape->geometry.get())) {
			buckets.erase(bucketEntry);
			return;
		}

		proxyBuckets.emplace(bucket.asset.proxy.get(), &bucket);
		const float meshCost = std::sqrt(std::max(1.0f, (float)bucket.asset.key.triCount / 8.0f));
		bucket.distScale = 1.0f / meshCost;
		bucket.minPixelScale = meshCost;
	}

	if (!shape.admitted) {
		activeShapes.push_back(capture.shape);
		shape.admitted = true;
	}

	if (affected.contains(&bucket))
		PruneSlices(bucket);

	if (shape.current && shape.current != &bucket) {
		affected.insert(shape.current);
		MigrateShapeSlices(*shape.current, bucket, shape);
	}

	shape.current = &bucket;
	if (auto previous = shape.chunks.find(capture.slice.engineGroup); previous != shape.chunks.end()) {
		previous->second->bucket = nullptr;
		PruneSlices(bucket);
		retired.push_back(std::move(previous->second));
		shape.chunks.erase(previous);
	}

	capture.slice.fadeStart = frameParams.fadeStart;

	auto chunk = std::make_unique<GrassChunk>(std::move(capture.slice));
	AppendSlice(bucket, *chunk);
	shape.chunks.emplace(chunk->engineGroup, std::move(chunk));
	snapshotDirty = true;
}

void GrassBucketStore::ApplyRemoval(const Removal& removal, std::unordered_set<GrassBucket*>& affected, std::vector<std::unique_ptr<GrassChunk>>& retired)
{
	auto& shape = *removal.shape;
	shape.pendingChanges.fetch_sub(1, std::memory_order_release);
	const auto detach = [&](std::unique_ptr<GrassChunk>& chunk) {
		affected.insert(chunk->bucket);
		chunk->bucket = nullptr;
		retired.push_back(std::move(chunk));
	};

	if (removal.engineGroup == UINT32_MAX) {
		for (auto& [group, chunk] : shape.chunks)
			detach(chunk);
		shape.chunks.clear();
	} else if (auto chunk = shape.chunks.find(removal.engineGroup); chunk != shape.chunks.end() && chunk->second->lifetime == removal.lifetime) {
		detach(chunk->second);
		shape.chunks.erase(chunk);
	}

	if (shape.chunks.empty())
		shape.current = nullptr;

	snapshotDirty = true;
}

void GrassBucketStore::AppendSlice(GrassBucket& bucket, GrassChunk& slice)
{
	maintenance.insert(&bucket);

	slice.bucket = &bucket;
	slice.bufferOffset = UINT32_MAX;
	slice.firstCullGroup = UINT32_MAX;

	if (slice.maxScale > bucket.maxScale) {
		bucket.maxScale = slice.maxScale;
		bucket.coarseValid = false;
		bucket.clustersValid = false;
	}

	const auto& bounds = slice.bounds;

	// Extend valid bounds here and recompute invalidated bounds after removals.
	if (bucket.coarseValid || bucket.slices.empty()) {
		const float padding = bucket.boundsPadding;
		const RE::NiPoint3 lo{ bounds.lo[0] - padding, bounds.lo[1] - padding, bounds.lo[2] - padding };
		const RE::NiPoint3 hi{ bounds.hi[0] + padding, bounds.hi[1] + padding, bounds.hi[2] + padding };

		if (bucket.slices.empty()) {
			bucket.coarseMin = lo;
			bucket.coarseMax = hi;
		} else {
			bucket.coarseMin.x = std::min(bucket.coarseMin.x, lo.x);
			bucket.coarseMin.y = std::min(bucket.coarseMin.y, lo.y);
			bucket.coarseMin.z = std::min(bucket.coarseMin.z, lo.z);
			bucket.coarseMax.x = std::max(bucket.coarseMax.x, hi.x);
			bucket.coarseMax.y = std::max(bucket.coarseMax.y, hi.y);
			bucket.coarseMax.z = std::max(bucket.coarseMax.z, hi.z);
		}

		bucket.coarseValid = true;
	}

	bucket.totalInstances += slice.count;
	bucket.slices.push_back(&slice);
}

void GrassBucketStore::PruneSlices(GrassBucket& bucket)
{
	const size_t originalCount = bucket.slices.size();
	size_t retainedCount = 0;
	uint32_t removedInstances = 0;
	uint32_t removedClusteredSlices = 0;
	uint32_t removedUploadedSlices = 0;

	for (size_t index = 0; index < originalCount; ++index) {
		auto& slice = *bucket.slices[index];
		if (slice.bucket == &bucket) {
			if (retainedCount != index)
				bucket.slices[retainedCount] = &slice;
			++retainedCount;
			continue;
		}

		removedInstances += slice.count;
		if (index < bucket.uploadedSlices) {
			++removedUploadedSlices;
			UpdateShapeResidency(slice, false);
		}

		if (index >= bucket.clusteredSlices)
			continue;

<<<<<<< HEAD
void GrassBucketStore::ApplyCaptures(std::vector<PendingCapture>& captures)
{
	// Queue pending adds so additions can be processed under one lock
	static thread_local std::vector<std::pair<RE::BSMultiStreamInstanceTriShape*, GrassBucket*>> pendingMapAdds;
	pendingMapAdds.clear();

	auto* ctx = globals::d3d::context;

	for (auto& pc : captures) {
		const uint32_t meshId = meshLibrary.ResolveMeshId(pc.shape);
		const uint32_t triCount = meshId ? 0u : pc.triangleCount;
		const BucketKey bk{ meshId, pc.material, meshId ? nullptr : pc.diffuseTexture.get(), triCount, meshId ? 0u : pc.descVal };
		auto& b = buckets[bk];
		b.meshId = meshId;
		b.diffuseTexture = pc.diffuseTexture;

		if (b.firstNewSlice == UINT32_MAX)
			b.firstNewSlice = (uint32_t)b.slices.size();

		if (!b.typeParamsValid) {
			CacheBucketTypeParams(b, pc);
			b.isComplex = DetectComplexGrass(pc.diffuseTexture.get(), ctx);
		}
		if (frameParams.enableMeshLOD)
			meshLibrary.EnsureLODMeshes(meshId);

		BucketSlice s;
		s.shape = pc.shape;
		s.count = pc.count;
		s.fadeStart = frameParams.fadeStart;
		s.origin = pc.origin;
		s.localMin = pc.localMin;
		s.localMax = pc.localMax;
		s.data = std::move(pc.bytes);

		SliceBounds sb;
		sb.lo[0] = pc.origin.x + pc.localMin.x;
		sb.lo[1] = pc.origin.y + pc.localMin.y;
		sb.lo[2] = pc.origin.z + pc.localMin.z;
		sb.hi[0] = pc.origin.x + pc.localMax.x;
		sb.hi[1] = pc.origin.y + pc.localMax.y;
		sb.hi[2] = pc.origin.z + pc.localMax.z;

		b.totalInstances += pc.count;
		b.slices.push_back(std::move(s));
		if (pc.shape)
			pendingMapAdds.emplace_back(pc.shape, &b);
		b.sliceBounds.push_back(sb);
		b.clustersValid = false;
		b.coarseValid = false;
	}

	if (!pendingMapAdds.empty()) {
		std::unique_lock lk(shapeBucketMutex);
		for (const auto& [shape, bucket] : pendingMapAdds)
			shapeBucketId[shape] = bucket;
	}
}

bool GrassBucketStore::CompactBucket(GrassBucket& b, ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	if (!b.instanceBuf || !b.originBuf || !b.capacityInstances)
		return false;

	const uint32_t surviving = (b.firstNewSlice == UINT32_MAX) ? (uint32_t)b.slices.size() : b.firstNewSlice;

	// Split the survivors which have fragmented by removals into runs of still contiguous offsets, so they can be copied back into a new contiguous buffer.
	struct Run
	{
		uint32_t oldFirst;
		uint32_t newFirst;
		uint32_t instances;
	};
	static thread_local std::vector<Run> runs;
	runs.clear();

	uint32_t newOffset = 0;
	for (uint32_t i = 0; i < surviving; ++i) {
		const BucketSlice& s = b.slices[i];
		if (s.bufferOffset == UINT32_MAX || !s.count)
			return false;  // never uploaded, so there is nothing resident to shift

		if (!runs.empty() && s.bufferOffset == runs.back().oldFirst + runs.back().instances)
			runs.back().instances += s.count;
		else
			runs.push_back({ s.bufferOffset, newOffset, s.count });

		newOffset += s.count;
	}

	if (runs.empty())
		return false;

	// Only trailing slices went, so everything still resident is already where it belongs.
	if (runs.size() == 1 && runs[0].oldFirst == 0)
		return true;

	// D3D11 doesn't allow overlapping copies within one resource, so shift the survivors into a new buffer and swap it in.
	ID3D11Buffer* oldInstance = b.instanceBuf;
	ID3D11Buffer* oldOrigin = b.originBuf;
	ID3D11ShaderResourceView* oldInstanceSRV = b.instanceSRV;
	ID3D11ShaderResourceView* oldOriginSRV = b.originSRV;
	b.instanceBuf = nullptr;
	b.originBuf = nullptr;
	b.instanceSRV = nullptr;
	b.originSRV = nullptr;

	if (!CreateBucketSourceBuffers(b, b.capacityInstances, device)) {
		auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
		rel(b.instanceBuf);
		rel(b.originBuf);
		rel(b.instanceSRV);
		rel(b.originSRV);
		b.instanceBuf = oldInstance;
		b.originBuf = oldOrigin;
		b.instanceSRV = oldInstanceSRV;
		b.originSRV = oldOriginSRV;
		return false;  // offsets are untouched, so a CPU rebuild is still valid
	}

	constexpr UINT kRecordBytes = kGrassStride;
	constexpr UINT kOriginBytes = 4 * (UINT)sizeof(float);
	for (const Run& run : runs) {
		const D3D11_BOX ibox{ run.oldFirst * kRecordBytes, 0, 0, (run.oldFirst + run.instances) * kRecordBytes, 1, 1 };
		ctx->CopySubresourceRegion(b.instanceBuf, 0, run.newFirst * kRecordBytes, 0, 0, oldInstance, 0, &ibox);

		const D3D11_BOX obox{ run.oldFirst * kOriginBytes, 0, 0, (run.oldFirst + run.instances) * kOriginBytes, 1, 1 };
		ctx->CopySubresourceRegion(b.originBuf, 0, run.newFirst * kOriginBytes, 0, 0, oldOrigin, 0, &obox);
	}

	newOffset = 0;
	for (uint32_t i = 0; i < surviving; ++i) {
		b.slices[i].bufferOffset = newOffset;
		newOffset += b.slices[i].count;
	}

	oldInstanceSRV->Release();
	oldOriginSRV->Release();
	oldInstance->Release();
	oldOrigin->Release();
	return true;
}

void GrassBucketStore::UploadDirtyBuckets(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	for (auto& [key, b] : buckets) {
		if (b.needsCompact) {
			b.needsCompact = false;
			if (CompactBucket(b, device, ctx)) {
				b.rebuildFromSlice = UINT32_MAX;  // survivors sit contiguously again
			} else {
				b.dirty = true;
			}
		}

		if (b.dirty) {
			RebuildBucket(b, device, ctx);
		} else if (b.firstNewSlice != UINT32_MAX) {
			if (b.totalInstances > b.capacityInstances)
				RebuildBucket(b, device, ctx);
			else
				AppendNewSlices(b, ctx);
||||||| 12b261724
void GrassBucketStore::ApplyCaptures(std::vector<PendingCapture>& captures)
{
	// Queue pending adds so additions can be processed under one lock
	static thread_local std::vector<std::pair<RE::BSMultiStreamInstanceTriShape*, GrassBucket*>> pendingMapAdds;
	pendingMapAdds.clear();

	auto* ctx = globals::d3d::context;

	for (auto& pc : captures) {
		const uint32_t meshId = meshLibrary.ResolveMeshId(pc.shape);
		const uint32_t triCount = meshId ? 0u : (uint32_t)pc.shape->GetTrishapeRuntimeData().triangleCount;
		const BucketKey bk{ meshId, pc.material, meshId ? nullptr : pc.diffuseTexture, triCount, meshId ? 0u : pc.descVal };
		auto& b = buckets[bk];
		b.meshId = meshId;
		b.diffuseTexture = RE::NiPointer<RE::NiSourceTexture>(pc.diffuseTexture);

		if (b.firstNewSlice == UINT32_MAX)
			b.firstNewSlice = (uint32_t)b.slices.size();

		if (!b.typeParamsValid) {
			CacheBucketTypeParams(b, pc.shape);
			b.isComplex = DetectComplexGrass(pc.diffuseTexture, ctx);
		}
		if (frameParams.enableMeshLOD)
			meshLibrary.EnsureLODMeshes(meshId);

		BucketSlice s;
		s.shape = pc.shape;
		s.count = pc.count;
		s.fadeStart = frameParams.fadeStart;
		s.origin = pc.origin;
		s.localMin = pc.localMin;
		s.localMax = pc.localMax;
		s.data = std::move(pc.bytes);

		SliceBounds sb;
		sb.lo[0] = pc.origin.x + pc.localMin.x;
		sb.lo[1] = pc.origin.y + pc.localMin.y;
		sb.lo[2] = pc.origin.z + pc.localMin.z;
		sb.hi[0] = pc.origin.x + pc.localMax.x;
		sb.hi[1] = pc.origin.y + pc.localMax.y;
		sb.hi[2] = pc.origin.z + pc.localMax.z;

		b.totalInstances += pc.count;
		b.slices.push_back(std::move(s));
		if (pc.shape)
			pendingMapAdds.emplace_back(pc.shape, &b);
		b.sliceBounds.push_back(sb);
		b.clustersValid = false;
		b.coarseValid = false;
	}

	if (!pendingMapAdds.empty()) {
		std::unique_lock lk(shapeBucketMutex);
		for (const auto& [shape, bucket] : pendingMapAdds)
			shapeBucketId[shape] = bucket;
	}
}

bool GrassBucketStore::CompactBucket(GrassBucket& b, ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	if (!b.instanceBuf || !b.originBuf || !b.capacityInstances)
		return false;

	const uint32_t surviving = (b.firstNewSlice == UINT32_MAX) ? (uint32_t)b.slices.size() : b.firstNewSlice;

	// Split the survivors which have fragmented by removals into runs of still contiguous offsets, so they can be copied back into a new contiguous buffer.
	struct Run
	{
		uint32_t oldFirst;
		uint32_t newFirst;
		uint32_t instances;
	};
	static thread_local std::vector<Run> runs;
	runs.clear();

	uint32_t newOffset = 0;
	for (uint32_t i = 0; i < surviving; ++i) {
		const BucketSlice& s = b.slices[i];
		if (s.bufferOffset == UINT32_MAX || !s.count)
			return false;  // never uploaded, so there is nothing resident to shift

		if (!runs.empty() && s.bufferOffset == runs.back().oldFirst + runs.back().instances)
			runs.back().instances += s.count;
		else
			runs.push_back({ s.bufferOffset, newOffset, s.count });

		newOffset += s.count;
	}

	if (runs.empty())
		return false;

	// Only trailing slices went, so everything still resident is already where it belongs.
	if (runs.size() == 1 && runs[0].oldFirst == 0)
		return true;

	// D3D11 doesn't allow overlapping copies within one resource, so shift the survivors into a new buffer and swap it in.
	ID3D11Buffer* oldInstance = b.instanceBuf;
	ID3D11Buffer* oldOrigin = b.originBuf;
	ID3D11ShaderResourceView* oldInstanceSRV = b.instanceSRV;
	ID3D11ShaderResourceView* oldOriginSRV = b.originSRV;
	b.instanceBuf = nullptr;
	b.originBuf = nullptr;
	b.instanceSRV = nullptr;
	b.originSRV = nullptr;

	if (!CreateBucketSourceBuffers(b, b.capacityInstances, device)) {
		auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
		rel(b.instanceBuf);
		rel(b.originBuf);
		rel(b.instanceSRV);
		rel(b.originSRV);
		b.instanceBuf = oldInstance;
		b.originBuf = oldOrigin;
		b.instanceSRV = oldInstanceSRV;
		b.originSRV = oldOriginSRV;
		return false;  // offsets are untouched, so a CPU rebuild is still valid
	}

	constexpr UINT kRecordBytes = kGrassStride;
	constexpr UINT kOriginBytes = 4 * (UINT)sizeof(float);
	for (const Run& run : runs) {
		const D3D11_BOX ibox{ run.oldFirst * kRecordBytes, 0, 0, (run.oldFirst + run.instances) * kRecordBytes, 1, 1 };
		ctx->CopySubresourceRegion(b.instanceBuf, 0, run.newFirst * kRecordBytes, 0, 0, oldInstance, 0, &ibox);

		const D3D11_BOX obox{ run.oldFirst * kOriginBytes, 0, 0, (run.oldFirst + run.instances) * kOriginBytes, 1, 1 };
		ctx->CopySubresourceRegion(b.originBuf, 0, run.newFirst * kOriginBytes, 0, 0, oldOrigin, 0, &obox);
	}

	newOffset = 0;
	for (uint32_t i = 0; i < surviving; ++i) {
		b.slices[i].bufferOffset = newOffset;
		newOffset += b.slices[i].count;
	}

	oldInstanceSRV->Release();
	oldOriginSRV->Release();
	oldInstance->Release();
	oldOrigin->Release();
	return true;
}

void GrassBucketStore::UploadDirtyBuckets(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	for (auto& [key, b] : buckets) {
		if (b.needsCompact) {
			b.needsCompact = false;
			if (CompactBucket(b, device, ctx)) {
				b.rebuildFromSlice = UINT32_MAX;  // survivors sit contiguously again
			} else {
				b.dirty = true;
			}
		}

		if (b.dirty) {
			RebuildBucket(b, device, ctx);
		} else if (b.firstNewSlice != UINT32_MAX) {
			if (b.totalInstances > b.capacityInstances)
				RebuildBucket(b, device, ctx);
			else
				AppendNewSlices(b, ctx);
=======
		++removedClusteredSlices;
		// Keep removed groups in place with zero counts until the cached runs are rebuilt.
		if (bucket.clustersValid && slice.firstCullGroup != UINT32_MAX) {
			const uint32_t groupCount = (slice.count + kCullGroupSize - 1) / kCullGroupSize;
			for (uint32_t groupIndex = 0; groupIndex < groupCount; ++groupIndex)
				bucket.cullGroups[slice.firstCullGroup + groupIndex].count = 0;
			bucket.deadGroups += groupCount;
			bucket.dirtyGroupRanges.emplace_back(slice.firstCullGroup, slice.firstCullGroup + groupCount);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
		}
	}

	if (retainedCount == originalCount)
		return;

	bucket.slices.resize(retainedCount);

	bucket.uploadedSlices -= removedUploadedSlices;

	bucket.clusteredSlices -= removedClusteredSlices;
	if (bucket.deadGroups > bucket.cullGroups.size() / 2)
		bucket.clustersValid = false;

	bucket.totalInstances -= removedInstances;

<<<<<<< HEAD
	StageCapture(shape, instanceData, count, stride, descVal, tex);
}

bool GrassBucketStore::StageCapture(RE::BSMultiStreamInstanceTriShape* shape, const void* src, uint32_t count, uint32_t stride, uint64_t descVal, RE::NiSourceTexture* tex)
{
	if (!shape || !src || !tex || !count || stride != kGrassStride) {
		logger::debug("[GRASS OPTIMIZATIONS] capture rejected: count={} stride={} desc={:016X} shape={:p}", count, stride, descVal, (void*)shape);
		return false;
	}
	auto shaderProperty = shape->GetGeometryRuntimeData().shaderProperty;
	if (!shaderProperty || shaderProperty->GetRTTI() != globals::rtti::BSGrassShaderPropertyRTTI.get())
		return false;
	auto* grassProperty = static_cast<RE::BSGrassShaderProperty*>(shaderProperty.get());
	auto* material = grassProperty->material;
	if (!material)
		return false;

	const auto& bound = shape->GetModelData().modelBound;

	PendingCapture pc;
	pc.shape = shape;
	pc.material = material;
	pc.descVal = descVal;
	pc.diffuseTexture = RE::NiPointer<RE::NiSourceTexture>(tex);
	pc.count = count;
	pc.triangleCount = shape->GetTrishapeRuntimeData().triangleCount;
	pc.boundCenter = bound.center;
	pc.modelRadius = bound.radius;
	pc.wavePeriod = grassProperty->wavePeriod;
	pc.origin = shape->world.translate;
	pc.bytes.resize((size_t)count * kGrassStride);
	std::memcpy(pc.bytes.data(), src, pc.bytes.size());

	// Read the instance coordinates from the source allocation. Store the local min/max in the slice so the coarse bounds can be computed without re-reading the instance data.
	{
		using DirectX::PackedVector::XMConvertHalfToFloat;
		RE::NiPoint3 lmn{ FLT_MAX, FLT_MAX, FLT_MAX };
		RE::NiPoint3 lmx{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
		const uint8_t* data = pc.bytes.data();
		for (uint32_t i = 0; i < count; ++i) {
			const uint8_t* cord = data + (size_t)i * kGrassStride;
			const float lx = XMConvertHalfToFloat(*reinterpret_cast<const uint16_t*>(cord + 0));
			const float ly = XMConvertHalfToFloat(*reinterpret_cast<const uint16_t*>(cord + 2));
			const float lz = XMConvertHalfToFloat(*reinterpret_cast<const uint16_t*>(cord + 4));
			lmn.x = std::min(lmn.x, lx);
			lmn.y = std::min(lmn.y, ly);
			lmn.z = std::min(lmn.z, lz);
			lmx.x = std::max(lmx.x, lx);
			lmx.y = std::max(lmx.y, ly);
			lmx.z = std::max(lmx.z, lz);
		}
		pc.localMin = lmn;
		pc.localMax = lmx;
	}

	std::scoped_lock lk(pendingMutex);
	pendingCaptures.push_back(std::move(pc));
	return true;
}

void GrassBucketStore::CacheBucketTypeParams(GrassBucket& b, const PendingCapture& pc)
{
	if (b.typeParamsValid)
		return;

	b.wavePeriod = pc.wavePeriod;
	b.boundCenter = pc.boundCenter;
	b.modelRadius = pc.modelRadius;

	const float tris = (float)pc.triangleCount;
	const float cost = std::max(1.0f, tris / 8.0f);
	const float w = std::sqrt(cost);
	b.distScale = 1.0f / w;
	b.minPixelScale = w;
	b.typeParamsValid = true;
}

void GrassBucketStore::RebuildBucket(GrassBucket& bucket, ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	// Rebuild only from the tail. Do a full rebuild when both are UINT32_MAX, or when the first stale slice is not contiguous with the previous slices.
	uint32_t from = std::min(bucket.rebuildFromSlice, bucket.firstNewSlice);
	if (from == UINT32_MAX || from > (uint32_t)bucket.slices.size())
		from = 0;

	// A partial rebuild is only valid if everything below `from` sits contiguously.
	uint32_t startInstance = 0;
	for (uint32_t i = 0; i < from; ++i) {
		const auto& s = bucket.slices[i];
		if (s.bufferOffset == UINT32_MAX || s.bufferOffset != startInstance) {
			from = 0;
			startInstance = 0;
			break;
		}
		startInstance += s.count;
	}

	// Copy from the old buffer starting with the given start instance to the new buffer on the GPU, so the rebuild below still only uploads the tail.
	if (!EnsureBucketCapacity(bucket, bucket.totalInstances, device, ctx, startInstance))
		return;

	if (!bucket.totalInstances) {
		bucket.dirty = false;
		bucket.rebuildFromSlice = UINT32_MAX;
		bucket.firstNewSlice = UINT32_MAX;
||||||| 12b261724
	StageCapture(shape, instanceData, count, stride, descVal, tex);
}

bool GrassBucketStore::StageCapture(RE::BSMultiStreamInstanceTriShape* shape, const void* src, uint32_t count, uint32_t stride, uint64_t descVal, RE::NiSourceTexture* tex)
{
	if (!shape || !src || !tex || !count || stride != kGrassStride) {
		logger::debug("[GRASS OPTIMIZATIONS] capture rejected: count={} stride={} desc={:016X} shape={:p}", count, stride, descVal, (void*)shape);
		return false;
	}
	auto shaderProperty = shape->GetGeometryRuntimeData().shaderProperty;
	if (!shaderProperty || shaderProperty->GetRTTI() != globals::rtti::BSGrassShaderPropertyRTTI.get())
		return false;
	auto* material = static_cast<RE::BSGrassShaderProperty*>(shaderProperty.get())->material;
	if (!material)
		return false;

	PendingCapture pc;
	pc.shape = shape;
	pc.material = material;
	pc.descVal = descVal;
	pc.diffuseTexture = tex;
	pc.count = count;
	pc.origin = shape->world.translate;
	pc.bytes.resize((size_t)count * kGrassStride);
	std::memcpy(pc.bytes.data(), src, pc.bytes.size());

	// Read the instance coordinates from the source allocation. Store the local min/max in the slice so the coarse bounds can be computed without re-reading the instance data.
	{
		using DirectX::PackedVector::XMConvertHalfToFloat;
		RE::NiPoint3 lmn{ FLT_MAX, FLT_MAX, FLT_MAX };
		RE::NiPoint3 lmx{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
		const uint8_t* data = pc.bytes.data();
		for (uint32_t i = 0; i < count; ++i) {
			const uint8_t* cord = data + (size_t)i * kGrassStride;
			const float lx = XMConvertHalfToFloat(*reinterpret_cast<const uint16_t*>(cord + 0));
			const float ly = XMConvertHalfToFloat(*reinterpret_cast<const uint16_t*>(cord + 2));
			const float lz = XMConvertHalfToFloat(*reinterpret_cast<const uint16_t*>(cord + 4));
			lmn.x = std::min(lmn.x, lx);
			lmn.y = std::min(lmn.y, ly);
			lmn.z = std::min(lmn.z, lz);
			lmx.x = std::max(lmx.x, lx);
			lmx.y = std::max(lmx.y, ly);
			lmx.z = std::max(lmx.z, lz);
		}
		pc.localMin = lmn;
		pc.localMax = lmx;
	}

	std::scoped_lock lk(pendingMutex);
	pendingCaptures.push_back(std::move(pc));
	return true;
}

void GrassBucketStore::CacheBucketTypeParams(GrassBucket& b, RE::BSMultiStreamInstanceTriShape* shape)
{
	if (b.typeParamsValid || !shape)
		return;

	if (auto* prop = static_cast<RE::BSGrassShaderProperty*>(shape->GetGeometryRuntimeData().shaderProperty.get()))
		b.wavePeriod = prop->wavePeriod;

	const auto& bound = shape->GetModelData().modelBound;
	b.boundCenter = bound.center;
	b.modelRadius = bound.radius;

	const float tris = (float)shape->GetTrishapeRuntimeData().triangleCount;
	const float cost = std::max(1.0f, tris / 8.0f);
	const float w = std::sqrt(cost);
	b.distScale = 1.0f / w;
	b.minPixelScale = w;
	b.typeParamsValid = true;
}

void GrassBucketStore::RebuildBucket(GrassBucket& bucket, ID3D11Device* device, ID3D11DeviceContext* ctx)
{
	// Rebuild only from the tail. Do a full rebuild when both are UINT32_MAX, or when the first stale slice is not contiguous with the previous slices.
	uint32_t from = std::min(bucket.rebuildFromSlice, bucket.firstNewSlice);
	if (from == UINT32_MAX || from > (uint32_t)bucket.slices.size())
		from = 0;

	// A partial rebuild is only valid if everything below `from` sits contiguously.
	uint32_t startInstance = 0;
	for (uint32_t i = 0; i < from; ++i) {
		const auto& s = bucket.slices[i];
		if (s.bufferOffset == UINT32_MAX || s.bufferOffset != startInstance) {
			from = 0;
			startInstance = 0;
			break;
		}
		startInstance += s.count;
	}

	// Copy from the old buffer starting with the given start instance to the new buffer on the GPU, so the rebuild below still only uploads the tail.
	if (!EnsureBucketCapacity(bucket, bucket.totalInstances, device, ctx, startInstance))
		return;

	if (!bucket.totalInstances) {
		bucket.dirty = false;
		bucket.rebuildFromSlice = UINT32_MAX;
		bucket.firstNewSlice = UINT32_MAX;
=======
	if (bucket.slices.empty()) {
		bucket.residentInstances = 0;
		bucket.clusteredSlices = 0;
		bucket.clustersValid = false;
		bucket.coarseValid = false;
		bucket.fragmented = false;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
		return;
	}

	bucket.coarseValid = false;
	if (removedUploadedSlices) {
		bucket.fragmented = true;
		maintenance.insert(&bucket);
	}
}

void GrassBucketStore::MigrateShapeSlices(GrassBucket& source, GrassBucket& destination, GrassShape& shape)
{
	std::vector<GrassChunk*> migratingSlices;
	for (auto* chunk : source.slices) {
		if (chunk->bucket == &source && chunk->owner == &shape) {
			chunk->bucket = nullptr;
			migratingSlices.push_back(chunk);
		}
	}

	PruneSlices(source);

	for (auto* chunk : migratingSlices)
		AppendSlice(destination, *chunk);
}

void GrassBucketStore::EraseBucket(GrassBucket& bucket)
{
	maintenance.erase(&bucket);
	meshLibrary.ReleaseAsset(bucket.asset);
	retiredProxies.push_back(bucket.asset.proxy);
	proxyBuckets.erase(bucket.asset.proxy.get());
	buckets.erase(bucket.asset.key);
}

GrassBucket* GrassBucketStore::FindBucket(const RE::BSGeometry* shape)
{
	const auto entry = proxyBuckets.find(shape);
	return entry == proxyBuckets.end() ? nullptr : entry->second;
}

std::shared_ptr<GrassShape> GrassBucketStore::FindShape(RE::BSMultiStreamInstanceTriShape* shape)
{
	std::scoped_lock lock(pendingMutex);
	const auto entry = shapes.find(shape);
	return entry == shapes.end() ? nullptr : entry->second;
}

bool GrassBucketStore::IsShapeOptimized(const CullSnapshot::NativeShape& native, uint32_t frame)
{
	// An unregistered shape's native geometry may already be released, so it is never read.
	const auto& shape = *native.registration;
	if (!shape.registered.load(std::memory_order_acquire))
		return false;

	// A group replaced mid-frame keeps the group count, so staged changes are checked on every call.
	if (shape.pendingChanges.load(std::memory_order_acquire))
		return false;

	const uint32_t groups = shape.native->GetMultiStreamTrishapeRuntimeData().activeGroupCount;
	if (native.optimizedFrame != frame || native.optimizedGroups != groups) {
		native.optimizedFrame = frame;
		native.optimizedGroups = groups;
		native.optimized = IsShapeOptimized(shape);
	}
	return native.optimized;
}

bool GrassBucketStore::IsShapeOptimized(const GrassShape& shape)
{
	const auto& native = shape.native->GetMultiStreamTrishapeRuntimeData();
	// Cached groups do not update the engine's total instance count.
	return !shape.pendingChanges.load(std::memory_order_acquire) && shape.current && shape.current->optimizedReady && shape.residentGroups == native.activeGroupCount &&
	       shape.chunks.size() == native.activeGroupCount;
}

bool GrassBucketStore::IsGroupOptimized(const GrassShape& shape, uint32_t group)
{
	if (!shape.current || !shape.current->optimizedReady)
		return false;

	const auto entry = shape.chunks.find(group);
	if (entry == shape.chunks.end())
		return false;

	const auto& chunk = *entry->second;
	return chunk.lifetime->current.load(std::memory_order_acquire) && chunk.bufferOffset != UINT32_MAX;
}

void GrassBucketStore::UpdateShapeResidency(const GrassChunk& slice, bool resident)
{
	auto& shape = *slice.owner;
	const bool wasResident = shape.residentInstances != 0;

	if (resident) {
		shape.residentInstances += slice.count;
		++shape.residentGroups;
	} else {
		shape.residentInstances -= slice.count;
		--shape.residentGroups;
	}

	if (wasResident != (shape.residentInstances != 0))
		snapshotDirty = true;
}

void GrassBucketStore::PublishCullSnapshot()
{
	if (!snapshotDirty)
		return;

	auto snapshot = std::make_unique<CullSnapshot>();
	for (const auto& shape : activeShapes) {
		const bool direct = shape->residentInstances && shape->current && shape->current->optimizedReady;
		snapshot->nativeShapes.emplace(shape->native, CullSnapshot::NativeShape{ shape, direct });
	}

	for (const auto& [key, bucket] : buckets) {
		if (!bucket.slices.empty())
			snapshot->proxies.push_back(bucket.asset.proxy);
	}

	retiredSnapshot = std::move(cullSnapshot);
	cullSnapshot = std::move(snapshot);
	publishedSnapshot.store(cullSnapshot.get(), std::memory_order_release);
	snapshotDirty = false;
}

<<<<<<< HEAD
// The largest per-eye instance record bounds every bucket allocation.
inline uint32_t MaxBucketInstances()
{
	constexpr uint32_t maxStride = std::max(kGrassStride, GrassBucket::kExtrasFloat4Count * 4u * static_cast<uint32_t>(sizeof(float)));
	return UINT32_MAX / (maxStride * (globals::game::isVR ? 2u : 1u));
}

bool GrassBucketStore::EnsureBucketCapacity(GrassBucket& b, uint32_t needed, ID3D11Device* device, ID3D11DeviceContext* ctx, uint32_t preserveInstances)
||||||| 12b261724
// The widest allocation is kGrassStride bytes per instance, so a larger capacity wraps its ByteWidth
// and silently under-allocates. Bounding capacityInstances here also bounds EnsureLODBin's cap.
constexpr uint32_t kMaxBucketInstances = UINT32_MAX / kGrassStride;

bool GrassBucketStore::EnsureBucketCapacity(GrassBucket& b, uint32_t needed, ID3D11Device* device, ID3D11DeviceContext* ctx, uint32_t preserveInstances)
=======
void GrassBucketStore::SetOptimizedReady(GrassBucket& bucket, bool ready)
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
{
<<<<<<< HEAD
	if (b.capacityInstances >= needed && b.instanceBuf)
		return true;

	const uint32_t maxInstances = MaxBucketInstances();
	if (needed > maxInstances) {
		logger::error("[GRASS OPTIMIZATIONS] bucket capacity rejected: needed={} max={}", needed, maxInstances);
		return false;
||||||| 12b261724
	if (b.capacityInstances >= needed && b.instanceBuf)
		return true;

	if (needed > kMaxBucketInstances) {
		logger::error("[GRASS OPTIMIZATIONS] bucket capacity rejected: needed={} max={}", needed, kMaxBucketInstances);
		return false;
=======
	if (bucket.optimizedReady != ready) {
		bucket.optimizedReady = ready;
		snapshotDirty = true;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	}
<<<<<<< HEAD

	uint32_t cap = b.capacityInstances ? b.capacityInstances : std::clamp(std::bit_ceil(needed), 256u, maxInstances);
	while (cap < needed)
		cap = std::min(cap * 2, maxInstances);

	// Hold the old instance/origin buffers across the reallocation so the data up to preserve can be copied to the new buffers on the GPU.
	const uint32_t preserve = std::min(preserveInstances, b.capacityInstances);
	struct OldBuffers
	{
		ID3D11Buffer* instance;
		ID3D11Buffer* origin;
		~OldBuffers()
		{
			if (instance)
				instance->Release();
			if (origin)
				origin->Release();
		}
	} old{ b.instanceBuf, b.originBuf };

	b.instanceBuf = nullptr;
	b.originBuf = nullptr;

	b.ReleaseResources();

	if (!CreateBucketSourceBuffers(b, cap, device) ||
		!CreateBucketCullScratch(b, cap, device) ||
		!CreateBucketArgsBuffer(b, device)) {
		b.ReleaseResources();
		b.capacityInstances = 0;
		// Ensure buffer offset is reset, so the next rebuild will not start from a stale offset. The rebuild will re-upload the whole bucket, so the offsets will be set correctly.
		for (auto& s : b.slices)
			s.bufferOffset = UINT32_MAX;
		return false;
	}

	// Carry the unchanged head across on the GPU, so the caller still uploads only the tail.
	if (preserve && ctx) {
		if (old.instance && b.instanceBuf) {
			const D3D11_BOX box{ 0, 0, 0, preserve * kGrassStride, 1, 1 };
			ctx->CopySubresourceRegion(b.instanceBuf, 0, 0, 0, 0, old.instance, 0, &box);
		}
		if (old.origin && b.originBuf) {
			const D3D11_BOX box{ 0, 0, 0, preserve * 4 * (UINT)sizeof(float), 1, 1 };
			ctx->CopySubresourceRegion(b.originBuf, 0, 0, 0, 0, old.origin, 0, &box);
		}
	}

	b.capacityInstances = cap;
	b.dirty = true;
	return true;
}

bool GrassBucketStore::CreateBucketSourceBuffers(GrassBucket& b, uint32_t capacity, ID3D11Device* device)
{
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = capacity * kGrassStride;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.instanceBuf)) || !b.instanceBuf) {
			logger::error("[GRASS OPTIMIZATIONS] instance buffer create failed bytes={}", bd.ByteWidth);
			return false;
		}
		Util::SetResourceName(b.instanceBuf, "GrassOptimizations::InstanceBuf");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_R32_TYPELESS;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
		sv.BufferEx.FirstElement = 0;
		sv.BufferEx.NumElements = capacity * 8;
		sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
		if (FAILED(device->CreateShaderResourceView(b.instanceBuf, &sv, &b.instanceSRV)) || !b.instanceSRV) {
			logger::error("[GRASS OPTIMIZATIONS] instance raw SRV create failed");
			return false;
		}
		Util::SetResourceName(b.instanceSRV, "GrassOptimizations::InstanceBuf SRV");
	}

	// Origin + spawn time.
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = capacity * 4 * sizeof(float);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = 4 * sizeof(float);
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.originBuf)) || !b.originBuf) {
			logger::error("[GRASS OPTIMIZATIONS] origin buffer create failed");
			return false;
		}
		Util::SetResourceName(b.originBuf, "GrassOptimizations::OriginBuf");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.NumElements = capacity;
		if (FAILED(device->CreateShaderResourceView(b.originBuf, &sv, &b.originSRV)) || !b.originSRV) {
			logger::error("[GRASS OPTIMIZATIONS] origin SRV create failed");
			return false;
		}
		Util::SetResourceName(b.originSRV, "GrassOptimizations::OriginBuf SRV");
	}

	return true;
}

bool GrassBucketStore::CreateBucketCullScratch(GrassBucket& b, uint32_t capacity, ID3D11Device* device)
{
	// On VR, eye 1's survivors land in the second half of these buffers (slot capacity..2*capacity-1).
	const uint32_t eyeMultiplier = globals::game::isVR ? 2u : 1u;
	const uint32_t allocCapacity = capacity * eyeMultiplier;

	// Compacted survivors, consumed as an instanced vertex stream by the draw.
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = allocCapacity * kGrassStride;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.compactedBuf)) || !b.compactedBuf) {
			logger::error("[GRASS OPTIMIZATIONS] compacted buffer create failed");
			return false;
		}
		Util::SetResourceName(b.compactedBuf, "GrassOptimizations::CompactedBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = allocCapacity * 8;
		uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(device->CreateUnorderedAccessView(b.compactedBuf, &uav, &b.compactedUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] compacted UAV create failed");
			return false;
		}
		Util::SetResourceName(b.compactedUAV, "GrassOptimizations::CompactedBuf UAV");
	}

	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = allocCapacity * GrassBucket::kExtrasFloat4Count * 4 * sizeof(float);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = 4 * sizeof(float);
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.extrasBuf)) || !b.extrasBuf) {
			logger::error("[GRASS OPTIMIZATIONS] extras buffer create failed");
			return false;
		}
		Util::SetResourceName(b.extrasBuf, "GrassOptimizations::ExtrasBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = allocCapacity * GrassBucket::kExtrasFloat4Count;
		if (FAILED(device->CreateUnorderedAccessView(b.extrasBuf, &uav, &b.extrasUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] extras UAV create failed");
			return false;
		}
		Util::SetResourceName(b.extrasUAV, "GrassOptimizations::ExtrasBuf UAV");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.NumElements = allocCapacity * GrassBucket::kExtrasFloat4Count;
		if (FAILED(device->CreateShaderResourceView(b.extrasBuf, &sv, &b.extrasSRV))) {
			logger::error("[GRASS OPTIMIZATIONS] extras SRV create failed");
			return false;
		}
		Util::SetResourceName(b.extrasSRV, "GrassOptimizations::ExtrasBuf SRV");
	}

	return true;
}

bool GrassBucketStore::CreateBucketArgsBuffer(GrassBucket& b, ID3D11Device* device)
{
	// On VR the buffer holds two back-to-back 32-byte args blocks (one per eye); see kArgsBlockStride.
	const uint32_t argsUint32Count = globals::game::isVR ? 16u : 8u;
	const uint32_t initArgs[16] = { 0 };
	D3D11_SUBRESOURCE_DATA init{ initArgs, 0, 0 };

	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth = argsUint32Count * sizeof(uint32_t);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

	if (FAILED(device->CreateBuffer(&bd, &init, &b.argsBuf)) || !b.argsBuf) {
		logger::error("[GRASS OPTIMIZATIONS] args buffer create failed");
		return false;
	}
	Util::SetResourceName(b.argsBuf, "GrassOptimizations::ArgsBuf");

	// Windows on to the instance count(s), with the shader's address 0 mapping to eye 0's, so clearing
	// the view resets the count(s) without disturbing indexCount or (on VR) eye 1's StartInstanceLocation.
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = DXGI_FORMAT_R32_TYPELESS;
	uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uav.Buffer.FirstElement = instanceCountOffset / sizeof(uint32_t);
	uav.Buffer.NumElements = globals::game::isVR ? ((kArgsBlockStride / sizeof(uint32_t)) + 1) : 1;
	uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	if (FAILED(device->CreateUnorderedAccessView(b.argsBuf, &uav, &b.argsUAV)) || !b.argsUAV) {
		logger::error("[GRASS OPTIMIZATIONS] args UAV create failed");
		return false;
	}
	Util::SetResourceName(b.argsUAV, "GrassOptimizations::ArgsBuf UAV");

	// One 4-byte slot per (eye, tier); see kLODCounterEyeStride in GrassCullingCS.hlsl.
	const uint32_t lodCounterSlots = (globals::game::isVR ? 2u : 1u) * (uint32_t)GrassMeshLibrary::LODTier::kCount;
	const uint32_t initLODCounts[2 * (size_t)GrassMeshLibrary::LODTier::kCount] = {};
	bd.ByteWidth = lodCounterSlots * sizeof(uint32_t);
	bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	init.pSysMem = initLODCounts;
	if (FAILED(device->CreateBuffer(&bd, &init, &b.lodCounterBuf)) || !b.lodCounterBuf) {
		logger::error("[GRASS OPTIMIZATIONS] LOD counter buffer create failed");
		return false;
	}
	Util::SetResourceName(b.lodCounterBuf, "GrassOptimizations::LODCounterBuf");

	uav.Buffer.FirstElement = 0;
	uav.Buffer.NumElements = lodCounterSlots;
	if (FAILED(device->CreateUnorderedAccessView(b.lodCounterBuf, &uav, &b.lodCounterUAV)) || !b.lodCounterUAV) {
		logger::error("[GRASS OPTIMIZATIONS] LOD counter UAV create failed");
		return false;
	}
	Util::SetResourceName(b.lodCounterUAV, "GrassOptimizations::LODCounterBuf UAV");

	return true;
}

void GrassBucketStore::UpdateCoarseBounds(GrassBucket& b)
{
	RE::NiPoint3 mn{ FLT_MAX, FLT_MAX, FLT_MAX };
	RE::NiPoint3 mx{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (const auto& s : b.slices) {
		mn.x = std::min(mn.x, s.origin.x + s.localMin.x);
		mn.y = std::min(mn.y, s.origin.y + s.localMin.y);
		mn.z = std::min(mn.z, s.origin.z + s.localMin.z);
		mx.x = std::max(mx.x, s.origin.x + s.localMax.x);
		mx.y = std::max(mx.y, s.origin.y + s.localMax.y);
		mx.z = std::max(mx.z, s.origin.z + s.localMax.z);
	}
	if (mn.x > mx.x) {  // no slices / no instances — leave an empty box
		mn = { 0.0f, 0.0f, 0.0f };
		mx = { 0.0f, 0.0f, 0.0f };
	}

	// Generously pad to prevent instances on the screen edge from being visibly culled.
	const float pad = b.modelRadius + 128.0f;
	mn.x -= pad;
	mn.y -= pad;
	mn.z -= pad;
	mx.x += pad;
	mx.y += pad;
	mx.z += pad;

	b.coarseMin = mn;
	b.coarseMax = mx;
	b.coarseValid = true;
}

bool GrassBucketStore::DetectComplexGrass(RE::NiSourceTexture* tex, ID3D11DeviceContext* ctx)
{
	auto* rt = tex ? tex->rendererTexture : nullptr;
	auto* resourceView = rt ? Util::AsReal(rt->resourceView) : nullptr;
	std::string key = tex && !tex->name.empty() ? std::string(tex->name.c_str()) : std::string{};
	if (auto it = key.empty() ? complexCache.end() : complexCache.find(key); it != complexCache.end()) {
		it->second.complex = std::abs(it->second.normalLength - 1.0f) < cachedComplexThreshold;
		return it->second.complex;
	}

	bool complex = false;
	float normalLength = 0.0f;

	if (GetDetectCS() && detectResult && detectStaging && resourceView) {
		UINT initialCount = 0;
		ID3D11UnorderedAccessView* resultUAV = detectResult->uav.get();
		ctx->CSSetUnorderedAccessViews(0, 1, &resultUAV, &initialCount);
		ctx->CSSetShader(detectCS, nullptr, 0);
		ctx->CSSetShaderResources(0, 1, &resourceView);
		ctx->Dispatch(1, 1, 1);

		ID3D11UnorderedAccessView* nullUAV = nullptr;
		ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		ID3D11ShaderResourceView* nullSRV = nullptr;
		ctx->CSSetShaderResources(0, 1, &nullSRV);

		// Stalls once per unique texture on load rather than every frame or requiring a CPU readback of the full texture.
		ctx->CopyResource(detectStaging->resource.get(), detectResult->resource.get());
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(ctx->Map(detectStaging->resource.get(), 0, D3D11_MAP_READ, 0, &m))) {
			// Compare using the decoded length from the shader to avoid requring a constant buffer
			std::memcpy(&normalLength, m.pData, sizeof(float));
			complex = std::abs(normalLength - 1.0f) < cachedComplexThreshold;
			ctx->Unmap(detectStaging->resource.get(), 0);
		}
	}

	if (!key.empty() && resourceView)
		complexCache.insert_or_assign(std::move(key), ComplexEntry{ normalLength, complex });
	return complex;
}

bool GrassBucketStore::EnsureLODBin(GrassBucket& b, GrassMeshLibrary::LODTier tier, ID3D11Device* device)
{
	GrassBucket::LODBin& bin = b.lodBins[(size_t)tier];
	const bool tierEnabled = (tier == GrassMeshLibrary::LODTier::kFar) ? frameParams.enableFarLOD : frameParams.enableMidLOD;

	if (!frameParams.enableMeshLOD || !tierEnabled || b.meshId == 0) {
		if (bin.capacityInstances)
			bin.Release();
		return false;
	}

	meshLibrary.EnsureLODMeshes(b.meshId);
	if (!meshLibrary.GetLODMesh(b.meshId, tier)) {
		if (bin.capacityInstances)
			bin.Release();
		return false;
	}

	const uint32_t cap = b.capacityInstances;
	if (!cap)
		return false;
	if (bin.capacityInstances >= cap && bin.compactedBuf)
		return true;

	// Worst case every survivor takes this tier's mesh, so the bin is sized like the full-detail one.
	bin.Release();

	const char* tierName = (tier == GrassMeshLibrary::LODTier::kFar) ? "far" : "middle";
	// On VR, eye 1's survivors land in the second half of these buffers (slot cap..2*cap-1).
	const uint32_t eyeMultiplier = globals::game::isVR ? 2u : 1u;
	const uint32_t allocCap = cap * eyeMultiplier;

	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = allocCap * kGrassStride;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &bin.compactedBuf)) || !bin.compactedBuf) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD compacted buffer create failed", tierName);
			bin.Release();
			return false;
		}
		Util::SetResourceName(bin.compactedBuf, "GrassOptimizations::LODCompactedBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = allocCap * 8;
		uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(device->CreateUnorderedAccessView(bin.compactedBuf, &uav, &bin.compactedUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD compacted UAV create failed", tierName);
			bin.Release();
			return false;
		}
		Util::SetResourceName(bin.compactedUAV, "GrassOptimizations::LODCompactedBuf UAV");
	}

	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = allocCap * GrassBucket::kExtrasFloat4Count * 4 * sizeof(float);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = 4 * sizeof(float);
		if (FAILED(device->CreateBuffer(&bd, nullptr, &bin.extrasBuf)) || !bin.extrasBuf) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD extras buffer create failed", tierName);
			bin.Release();
			return false;
		}
		Util::SetResourceName(bin.extrasBuf, "GrassOptimizations::LODExtrasBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = allocCap * GrassBucket::kExtrasFloat4Count;
		uav.Buffer.Flags = 0;
		if (FAILED(device->CreateUnorderedAccessView(bin.extrasBuf, &uav, &bin.extrasUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD extras UAV create failed", tierName);
			bin.Release();
			return false;
		}
		Util::SetResourceName(bin.extrasUAV, "GrassOptimizations::LODExtrasBuf UAV");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.NumElements = allocCap * GrassBucket::kExtrasFloat4Count;
		if (FAILED(device->CreateShaderResourceView(bin.extrasBuf, &sv, &bin.extrasSRV))) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD extras SRV create failed", tierName);
			bin.Release();
			return false;
		}
		Util::SetResourceName(bin.extrasSRV, "GrassOptimizations::LODExtrasBuf SRV");
	}

	{
		const uint32_t initArgs[16] = { 0 };
		D3D11_SUBRESOURCE_DATA init{ initArgs, 0, 0 };

		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = (globals::game::isVR ? 16u : 8u) * sizeof(uint32_t);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;

		if (FAILED(device->CreateBuffer(&bd, &init, &bin.argsBuf)) || !bin.argsBuf) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD args buffer create failed", tierName);
			bin.Release();
			return false;
		}
		Util::SetResourceName(bin.argsBuf, "GrassOptimizations::LODArgsBuf");
	}

	bin.capacityInstances = cap;
	return true;
||||||| 12b261724

	uint32_t cap = b.capacityInstances ? b.capacityInstances : std::clamp(std::bit_ceil(needed), 256u, kMaxBucketInstances);
	while (cap < needed)
		cap = std::min(cap * 2, kMaxBucketInstances);

	// Hold the old instance/origin buffers across the reallocation so the data up to preserve can be copied to the new buffers on the GPU.
	const uint32_t preserve = std::min(preserveInstances, b.capacityInstances);
	struct OldBuffers
	{
		ID3D11Buffer* instance;
		ID3D11Buffer* origin;
		~OldBuffers()
		{
			if (instance)
				instance->Release();
			if (origin)
				origin->Release();
		}
	} old{ b.instanceBuf, b.originBuf };

	b.instanceBuf = nullptr;
	b.originBuf = nullptr;

	b.ReleaseResources();

	if (!CreateBucketSourceBuffers(b, cap, device) ||
		!CreateBucketCullScratch(b, cap, device) ||
		!CreateBucketArgsBuffer(b, device)) {
		b.ReleaseResources();
		b.capacityInstances = 0;
		// Ensure buffer offset is reset, so the next rebuild will not start from a stale offset. The rebuild will re-upload the whole bucket, so the offsets will be set correctly.
		for (auto& s : b.slices)
			s.bufferOffset = UINT32_MAX;
		return false;
	}

	// Carry the unchanged head across on the GPU, so the caller still uploads only the tail.
	if (preserve && ctx) {
		if (old.instance && b.instanceBuf) {
			const D3D11_BOX box{ 0, 0, 0, preserve * kGrassStride, 1, 1 };
			ctx->CopySubresourceRegion(b.instanceBuf, 0, 0, 0, 0, old.instance, 0, &box);
		}
		if (old.origin && b.originBuf) {
			const D3D11_BOX box{ 0, 0, 0, preserve * 4 * (UINT)sizeof(float), 1, 1 };
			ctx->CopySubresourceRegion(b.originBuf, 0, 0, 0, 0, old.origin, 0, &box);
		}
	}

	b.capacityInstances = cap;
	b.dirty = true;
	return true;
}

bool GrassBucketStore::CreateBucketSourceBuffers(GrassBucket& b, uint32_t capacity, ID3D11Device* device)
{
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = capacity * kGrassStride;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.instanceBuf)) || !b.instanceBuf) {
			logger::error("[GRASS OPTIMIZATIONS] instance buffer create failed bytes={}", bd.ByteWidth);
			return false;
		}
		Util::SetResourceName(b.instanceBuf, "GrassOptimizations::InstanceBuf");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_R32_TYPELESS;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
		sv.BufferEx.FirstElement = 0;
		sv.BufferEx.NumElements = capacity * 8;
		sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
		if (FAILED(device->CreateShaderResourceView(b.instanceBuf, &sv, &b.instanceSRV)) || !b.instanceSRV) {
			logger::error("[GRASS OPTIMIZATIONS] instance raw SRV create failed");
			return false;
		}
		Util::SetResourceName(b.instanceSRV, "GrassOptimizations::InstanceBuf SRV");
	}

	// Origin + spawn time.
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = capacity * 4 * sizeof(float);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = 4 * sizeof(float);
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.originBuf)) || !b.originBuf) {
			logger::error("[GRASS OPTIMIZATIONS] origin buffer create failed");
			return false;
		}
		Util::SetResourceName(b.originBuf, "GrassOptimizations::OriginBuf");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.NumElements = capacity;
		if (FAILED(device->CreateShaderResourceView(b.originBuf, &sv, &b.originSRV)) || !b.originSRV) {
			logger::error("[GRASS OPTIMIZATIONS] origin SRV create failed");
			return false;
		}
		Util::SetResourceName(b.originSRV, "GrassOptimizations::OriginBuf SRV");
	}

	return true;
}

bool GrassBucketStore::CreateBucketCullScratch(GrassBucket& b, uint32_t capacity, ID3D11Device* device)
{
	// Compacted survivors, consumed as an instanced vertex stream by the draw.
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = capacity * kGrassStride;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.compactedBuf)) || !b.compactedBuf) {
			logger::error("[GRASS OPTIMIZATIONS] compacted buffer create failed");
			return false;
		}
		Util::SetResourceName(b.compactedBuf, "GrassOptimizations::CompactedBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = capacity * 8;
		uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(device->CreateUnorderedAccessView(b.compactedBuf, &uav, &b.compactedUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] compacted UAV create failed");
			return false;
		}
		Util::SetResourceName(b.compactedUAV, "GrassOptimizations::CompactedBuf UAV");
	}

	// Extras: [i*2+0] = {origin.xyz, isComplex}, [i*2+1] = {windCur, windPrev, fade, collision}.
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = capacity * 2 * 4 * sizeof(float);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = 4 * sizeof(float);
		if (FAILED(device->CreateBuffer(&bd, nullptr, &b.extrasBuf)) || !b.extrasBuf) {
			logger::error("[GRASS OPTIMIZATIONS] extras buffer create failed");
			return false;
		}
		Util::SetResourceName(b.extrasBuf, "GrassOptimizations::ExtrasBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = capacity * 2;
		if (FAILED(device->CreateUnorderedAccessView(b.extrasBuf, &uav, &b.extrasUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] extras UAV create failed");
			return false;
		}
		Util::SetResourceName(b.extrasUAV, "GrassOptimizations::ExtrasBuf UAV");

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.NumElements = capacity * 2;
		if (FAILED(device->CreateShaderResourceView(b.extrasBuf, &sv, &b.extrasSRV))) {
			logger::error("[GRASS OPTIMIZATIONS] extras SRV create failed");
			return false;
		}
		Util::SetResourceName(b.extrasSRV, "GrassOptimizations::ExtrasBuf SRV");
	}

	return true;
}

bool GrassBucketStore::CreateBucketArgsBuffer(GrassBucket& b, ID3D11Device* device)
{
	// 3 uints of padding, so that the instanceCount is UAV accessible, so the cull CS can write to it directly.
	// Then the 5-uint args block at instanceCountOffset.
	const uint32_t initArgs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	D3D11_SUBRESOURCE_DATA init{ initArgs, 0, 0 };

	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth = 8 * sizeof(uint32_t);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

	if (FAILED(device->CreateBuffer(&bd, &init, &b.argsBuf)) || !b.argsBuf) {
		logger::error("[GRASS OPTIMIZATIONS] args buffer create failed");
		return false;
	}
	Util::SetResourceName(b.argsBuf, "GrassOptimizations::ArgsBuf");

	// Windows on to the instance count, with the shader's address 0 mapping to it, so clearing the view resets the count without disturbing indexCount. 
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = DXGI_FORMAT_R32_TYPELESS;
	uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uav.Buffer.FirstElement = instanceCountOffset / sizeof(uint32_t);
	uav.Buffer.NumElements = 1;
	uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	if (FAILED(device->CreateUnorderedAccessView(b.argsBuf, &uav, &b.argsUAV)) || !b.argsUAV) {
		logger::error("[GRASS OPTIMIZATIONS] args UAV create failed");
		return false;
	}
	Util::SetResourceName(b.argsUAV, "GrassOptimizations::ArgsBuf UAV");

	const uint32_t initLODCounts[(size_t)GrassMeshLibrary::LODTier::kCount] = {};
	bd.ByteWidth = sizeof(initLODCounts);
	bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	init.pSysMem = initLODCounts;
	if (FAILED(device->CreateBuffer(&bd, &init, &b.lodCounterBuf)) || !b.lodCounterBuf) {
		logger::error("[GRASS OPTIMIZATIONS] LOD counter buffer create failed");
		return false;
	}
	Util::SetResourceName(b.lodCounterBuf, "GrassOptimizations::LODCounterBuf");

	uav.Buffer.FirstElement = 0;
	uav.Buffer.NumElements = (UINT)std::size(initLODCounts);
	if (FAILED(device->CreateUnorderedAccessView(b.lodCounterBuf, &uav, &b.lodCounterUAV)) || !b.lodCounterUAV) {
		logger::error("[GRASS OPTIMIZATIONS] LOD counter UAV create failed");
		return false;
	}
	Util::SetResourceName(b.lodCounterUAV, "GrassOptimizations::LODCounterBuf UAV");

	return true;
}

void GrassBucketStore::UpdateCoarseBounds(GrassBucket& b)
{
	RE::NiPoint3 mn{ FLT_MAX, FLT_MAX, FLT_MAX };
	RE::NiPoint3 mx{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (const auto& s : b.slices) {
		mn.x = std::min(mn.x, s.origin.x + s.localMin.x);
		mn.y = std::min(mn.y, s.origin.y + s.localMin.y);
		mn.z = std::min(mn.z, s.origin.z + s.localMin.z);
		mx.x = std::max(mx.x, s.origin.x + s.localMax.x);
		mx.y = std::max(mx.y, s.origin.y + s.localMax.y);
		mx.z = std::max(mx.z, s.origin.z + s.localMax.z);
	}
	if (mn.x > mx.x) {  // no slices / no instances — leave an empty box
		mn = { 0.0f, 0.0f, 0.0f };
		mx = { 0.0f, 0.0f, 0.0f };
	}

	// Generously pad to prevent instances on the screen edge from being visibly culled. 
	const float pad = b.modelRadius + 128.0f;
	mn.x -= pad;
	mn.y -= pad;
	mn.z -= pad;
	mx.x += pad;
	mx.y += pad;
	mx.z += pad;

	b.coarseMin = mn;
	b.coarseMax = mx;
	b.coarseValid = true;
}

bool GrassBucketStore::DetectComplexGrass(RE::NiSourceTexture* tex, ID3D11DeviceContext* ctx)
{
	auto* rt = tex ? tex->rendererTexture : nullptr;
	auto* resourceView = rt ? rt->resourceView : nullptr;
	std::string key = tex && !tex->name.empty() ? std::string(tex->name.c_str()) : std::string{};
	if (auto it = key.empty() ? complexCache.end() : complexCache.find(key); it != complexCache.end()) {
		it->second.complex = std::abs(it->second.normalLength - 1.0f) < cachedComplexThreshold;
		return it->second.complex;
	}

	bool complex = false;
	float normalLength = 0.0f;

	if (GetDetectCS() && detectResult && detectStaging && resourceView) {
		UINT initialCount = 0;
		ID3D11UnorderedAccessView* resultUAV = detectResult->uav.get();
		ctx->CSSetUnorderedAccessViews(0, 1, &resultUAV, &initialCount);
		ctx->CSSetShader(detectCS, nullptr, 0);
		ctx->CSSetShaderResources(0, 1, &resourceView);
		ctx->Dispatch(1, 1, 1);

		ID3D11UnorderedAccessView* nullUAV = nullptr;
		ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		ID3D11ShaderResourceView* nullSRV = nullptr;
		ctx->CSSetShaderResources(0, 1, &nullSRV);

		// Stalls once per unique texture on load rather than every frame or requiring a CPU readback of the full texture.
		ctx->CopyResource(detectStaging->resource.get(), detectResult->resource.get());
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(ctx->Map(detectStaging->resource.get(), 0, D3D11_MAP_READ, 0, &m))) {
			// Compare using the decoded length from the shader to avoid requring a constant buffer
			std::memcpy(&normalLength, m.pData, sizeof(float));
			complex = std::abs(normalLength - 1.0f) < cachedComplexThreshold;
			ctx->Unmap(detectStaging->resource.get(), 0);
		}
	}

	if (!key.empty() && resourceView)
		complexCache.insert_or_assign(std::move(key), ComplexEntry{ normalLength, complex });
	return complex;
}

bool GrassBucketStore::EnsureLODBin(GrassBucket& b, GrassMeshLibrary::LODTier tier, ID3D11Device* device)
{
	GrassBucket::LODBin& bin = b.lodBins[(size_t)tier];
	const bool tierEnabled = (tier == GrassMeshLibrary::LODTier::kFar) ? frameParams.enableFarLOD : frameParams.enableMidLOD;

	if (!frameParams.enableMeshLOD || !tierEnabled || b.meshId == 0) {
		if (bin.capacityInstances)
			bin.Release();
		return false;
	}

	meshLibrary.EnsureLODMeshes(b.meshId);
	if (!meshLibrary.GetLODMesh(b.meshId, tier)) {
		if (bin.capacityInstances)
			bin.Release();
		return false;
	}

	const uint32_t cap = b.capacityInstances;
	if (!cap)
		return false;
	if (bin.capacityInstances >= cap && bin.compactedBuf)
		return true;

	// Worst case every survivor takes this tier's mesh, so the bin is sized like the full-detail one.
	bin.Release();

	const char* tierName = (tier == GrassMeshLibrary::LODTier::kFar) ? "far" : "middle";

	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = cap * kGrassStride;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &bin.compactedBuf)) || !bin.compactedBuf) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD compacted buffer create failed", tierName);
			bin.Release();
			return false;
		}
		Util::SetResourceName(bin.compactedBuf, "GrassOptimizations::LODCompactedBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_TYPELESS;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = cap * 8;
		uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(device->CreateUnorderedAccessView(bin.compactedBuf, &uav, &bin.compactedUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD compacted UAV create failed", tierName);
			bin.Release();
			return false;
		}
	}

	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = cap * 2 * 4 * sizeof(float);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = 4 * sizeof(float);
		if (FAILED(device->CreateBuffer(&bd, nullptr, &bin.extrasBuf)) || !bin.extrasBuf) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD extras buffer create failed", tierName);
			bin.Release();
			return false;
		}
		Util::SetResourceName(bin.extrasBuf, "GrassOptimizations::LODExtrasBuf");

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = cap * 2;
		uav.Buffer.Flags = 0;
		if (FAILED(device->CreateUnorderedAccessView(bin.extrasBuf, &uav, &bin.extrasUAV))) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD extras UAV create failed", tierName);
			bin.Release();
			return false;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_UNKNOWN;
		sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		sv.Buffer.NumElements = cap * 2;
		if (FAILED(device->CreateShaderResourceView(bin.extrasBuf, &sv, &bin.extrasSRV))) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD extras SRV create failed", tierName);
			bin.Release();
			return false;
		}
	}

	{
		const uint32_t initArgs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		D3D11_SUBRESOURCE_DATA init{ initArgs, 0, 0 };

		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = 8 * sizeof(uint32_t);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;

		if (FAILED(device->CreateBuffer(&bd, &init, &bin.argsBuf)) || !bin.argsBuf) {
			logger::error("[GRASS OPTIMIZATIONS] {} LOD args buffer create failed", tierName);
			bin.Release();
			return false;
		}
		Util::SetResourceName(bin.argsBuf, "GrassOptimizations::LODArgsBuf");
	}

	bin.capacityInstances = cap;
	return true;
=======
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
}

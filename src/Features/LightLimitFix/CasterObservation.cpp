#include "CasterObservation.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include <bit>

namespace CasterObservation
{
	namespace
	{
		constexpr uint32_t kIdentitySlots = kCapacity * 2;
		constexpr uint32_t kMaximumIdentityProbes = 16;
		constexpr uint32_t kMaximumConsumerFrameLag = 1;
		constexpr uint32_t kSSEPositionBytes = sizeof(float) * 4;
		enum class Rejection : uint32_t
		{
			ObservationLimit, IdentityProbeLimit, RecordCapacity, NotTriShape, Skinned, MissingMaterial, ShadowDepthFlags,
			NonLightingMaterial, DeformationOrRefraction, AlphaTestOrBlend, GeometryType, UnsupportedPositionLayout, MissingPositions,
			FadedMaterial, MissingBuffers, VertexBufferUsage, IndexBufferUsage, BufferLayout, IndexRange, NonFiniteTransformOrBound, Count
		};
		constexpr std::array<const char*, static_cast<size_t>(Rejection::Count)> kRejectionNames{
			"observationLimit", "identityProbeLimit", "recordCapacity", "notTriShape", "skinned", "missingMaterial", "shadowDepthFlags",
			"nonLightingMaterial", "deformationOrRefraction", "alphaTestOrBlend", "geometryType", "unsupportedPositionLayout", "missingPositions",
			"fadedMaterial", "missingBuffers", "vertexBufferNotImmutable", "indexBufferNotImmutable", "bufferLayout", "indexRange", "nonFiniteTransformOrBound"
		};
		enum class Event : uint32_t
		{
			SelectCalls, SelectionDisabled, SelectionVR, SelectionRepeatedFrame, SelectionExpiredLease, Selections, EmptySelections,
			BeginCalls, BeginDisabled, BeginVR, BeginNested, BeginRepeatedFrame, BeginWrongFrame, BeginWrongSlot, CaptureStarts,
			QueueFull, CaptureEnds, EndWithoutCapture, CallbacksOutsideCapture, CallbacksInCapture, OverflowCallbacks, Duplicates,
			Admitted, AdmittedWithoutFullPrecisionFlag, EpochInvalidations, CaptureEpochMismatch, MissingLightIdentity, IncompleteSignals, Count
		};
		constexpr std::array<const char*, static_cast<size_t>(Event::Count)> kEventNames{
			"selectCalls", "selectionDisabled", "selectionVR", "selectionRepeatedFrame", "selectionExpiredLease", "selections", "emptySelections",
			"beginCalls", "beginDisabled", "beginVR", "beginNested", "beginRepeatedFrame", "beginWrongFrame", "beginWrongSlot", "captureStarts",
			"queueFull", "captureEnds", "endWithoutCapture", "callbacksOutsideCapture", "callbacksInCapture", "overflowCallbacks", "duplicates",
			"admitted", "admittedWithoutFullPrecisionFlag", "epochInvalidations", "captureEpochMismatch", "missingLightIdentity", "incompleteSignals"
		};
		std::atomic<bool> diagnosticsEnabled{ false };
		std::array<std::atomic<uint64_t>, kEventNames.size()> eventCounts{};
		std::array<std::atomic<uint64_t>, kRejectionNames.size()> rejectionCounts{};
		void Count(Event a_event)
		{
			if (diagnosticsEnabled.load(std::memory_order_relaxed)) eventCounts[static_cast<size_t>(a_event)].fetch_add(1, std::memory_order_relaxed);
		}
		enum class SlotState : uint32_t { Free, Writing, Ready, Reading };
		struct Slot
		{
			std::atomic<SlotState> state{ SlotState::Free };
			Batch batch;
			std::array<uintptr_t, kIdentitySlots> identities{};
			std::array<uint32_t, kRejectionNames.size()> rejections{};
			uint64_t callbacks = 0, overflowCallbacks = 0;
			uint32_t admittedWithoutFullPrecisionFlag = 0;
			bool countDiagnostics = false;
		};
		std::array<Slot, kQueueCapacity> slots;
		std::atomic<bool> enabled{ false };
		std::atomic<uint64_t> epoch{ 1 }, lightSerial{ 0 }, batchSerial{ 0 }, dropped{ 0 }, completed{ 0 };
		std::atomic<uint32_t> selectedFrame{ UINT32_MAX }, selectedSlot{ UINT32_MAX };
		std::atomic<uint32_t> consumerFrame{ UINT32_MAX };
		thread_local Slot* writing = nullptr;
		std::atomic<uint32_t> attemptedFrame{ UINT32_MAX };
		uint32_t cursor = 0;
		void Reject(Rejection a_reason)
		{
			++writing->batch.omissions;
			if (writing->countDiagnostics) ++writing->rejections[static_cast<size_t>(a_reason)];
		}
	}
	void SetDiagnosticsEnabled(bool a_enabled) { diagnosticsEnabled.store(a_enabled, std::memory_order_relaxed); }
	void SetEnabled(bool a_enabled)
	{
		if (a_enabled) consumerFrame.store(globals::state->frameCountAtomic.load(std::memory_order_relaxed), std::memory_order_release);
		if (enabled.exchange(a_enabled) != a_enabled) Invalidate();
	}
	void Invalidate() { Count(Event::EpochInvalidations); epoch.fetch_add(1, std::memory_order_acq_rel); }
	uint64_t Epoch() { return epoch.load(std::memory_order_acquire); }
	uint64_t NewLightGeneration()
	{
		if (enabled.load(std::memory_order_relaxed)) Invalidate();
		return lightSerial.fetch_add(1, std::memory_order_relaxed) + 1;
	}
	void Select(uint32_t a_frame, uint32_t a_first, uint32_t a_count)
	{
		Count(Event::SelectCalls);
		if (!enabled.load(std::memory_order_relaxed)) { Count(Event::SelectionDisabled); return; }
		if (globals::game::isVR) { Count(Event::SelectionVR); return; }
		if (selectedFrame.load() == a_frame) { Count(Event::SelectionRepeatedFrame); return; }
		if (a_frame - consumerFrame.load(std::memory_order_acquire) > kMaximumConsumerFrameLag) { Count(Event::SelectionExpiredLease); return; }
		Count(Event::Selections);
		if (!a_count) Count(Event::EmptySelections);
		selectedSlot.store(a_count ? a_first + cursor++ % a_count : UINT32_MAX);
		selectedFrame.store(a_frame);
	}
	bool Begin(uint32_t a_frame, uint32_t a_slot, uint64_t a_lightGeneration, uint32_t a_policy)
	{
		Count(Event::BeginCalls);
		if (!enabled.load(std::memory_order_relaxed)) { Count(Event::BeginDisabled); return false; }
		if (globals::game::isVR) { Count(Event::BeginVR); return false; }
		if (writing) { Count(Event::BeginNested); return false; }
		if (attemptedFrame.load(std::memory_order_relaxed) == a_frame) { Count(Event::BeginRepeatedFrame); return false; }
		if (selectedFrame.load() != a_frame) { Count(Event::BeginWrongFrame); return false; }
		if (selectedSlot.load() != a_slot) { Count(Event::BeginWrongSlot); return false; }
		attemptedFrame.store(a_frame, std::memory_order_relaxed);
		for (auto& slot : slots) {
			auto available = SlotState::Free;
			if (!slot.state.compare_exchange_strong(available, SlotState::Writing, std::memory_order_acquire)) continue;
			slot.batch.count = slot.batch.observations = slot.batch.omissions = slot.batch.duplicates = 0;
			slot.identities.fill(0);
			slot.countDiagnostics = diagnosticsEnabled.load(std::memory_order_relaxed);
			if (slot.countDiagnostics) { slot.rejections.fill(0); slot.callbacks = slot.overflowCallbacks = 0; slot.admittedWithoutFullPrecisionFlag = 0; }
			slot.batch.incomplete = slot.batch.overflow = false;
			slot.batch.scope = { epoch.load(), a_lightGeneration, ++batchSerial, a_frame, a_slot, a_policy };
			writing = &slot;
			Count(Event::CaptureStarts);
			return true;
		}
		++dropped;
		Count(Event::QueueFull);
		return false;
	}
	void Incomplete() { if (writing) { Count(Event::IncompleteSignals); writing->batch.incomplete = true; } }
	void Observe(RE::BSGeometry& a_geometry)
	{
		if (!writing) { Count(Event::CallbacksOutsideCapture); return; }
		auto& batch = writing->batch;
		if (writing->countDiagnostics) ++writing->callbacks;
		if (batch.overflow) { if (writing->countDiagnostics) ++writing->overflowCallbacks; return; }
		if (++batch.observations > kMaximumObservations) {
			if (writing->countDiagnostics) ++writing->rejections[static_cast<size_t>(Rejection::ObservationLimit)];
			batch.overflow = batch.incomplete = true; return;
		}
		const auto identity = reinterpret_cast<uintptr_t>(&a_geometry);
		const auto bucket = std::hash<uintptr_t>{}(identity) & (kIdentitySlots - 1);
		bool inserted = false;
		for (uint32_t probe = 0; probe < kMaximumIdentityProbes; ++probe) {
			auto& entry = writing->identities[(bucket + probe) & (kIdentitySlots - 1)];
			if (entry == identity) { ++batch.duplicates; return; }
			if (!entry) { entry = identity; inserted = true; break; }
		}
		if (!inserted || batch.count == kCapacity) {
			if (writing->countDiagnostics) ++writing->rejections[static_cast<size_t>(!inserted ? Rejection::IdentityProbeLimit : Rejection::RecordCapacity)];
			batch.overflow = batch.incomplete = true; return;
		}
		auto& data = a_geometry.GetGeometryRuntimeData();
		auto* property = data.shaderProperty.get();
		auto* shape = a_geometry.AsTriShape();
		if (!shape) { Reject(Rejection::NotTriShape); return; }
		if (data.skinInstance) { Reject(Rejection::Skinned); return; }
		if (!property || !property->material) { Reject(Rejection::MissingMaterial); return; }
		if (!property->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows, RE::BSShaderProperty::EShaderPropertyFlag::kZBufferWrite)) { Reject(Rejection::ShadowDepthFlags); return; }
		if (property->material->GetType() != RE::BSShaderMaterial::Type::kLighting) { Reject(Rejection::NonLightingMaterial); return; }
		if (property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim, RE::BSShaderProperty::EShaderPropertyFlag::kLODLandscape,
			RE::BSShaderProperty::EShaderPropertyFlag::kRefraction, RE::BSShaderProperty::EShaderPropertyFlag::kTempRefraction)) { Reject(Rejection::DeformationOrRefraction); return; }
		if (data.alphaProperty && (data.alphaProperty->GetAlphaTesting() || data.alphaProperty->GetAlphaBlending())) { Reject(Rejection::AlphaTestOrBlend); return; }
		if (a_geometry.GetType() != RE::BSGeometry::Type::kTriShape) { Reject(Rejection::GeometryType); return; }
		if (!data.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX)) { Reject(Rejection::MissingPositions); return; }
		const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
		const float alpha = material->materialAlpha;
		const float fade = property->fadeNode ? property->fadeNode->GetRuntimeData().currentFade : 1.0f;
		if (!std::isfinite(alpha) || !std::isfinite(fade) || alpha * fade < 1.0f) { Reject(Rejection::FadedMaterial); return; }
		auto* mesh = static_cast<RE::BSGraphics::TriShape*>(data.rendererData);
		if (!mesh || !mesh->vertexBuffer || !mesh->indexBuffer) { Reject(Rejection::MissingBuffers); return; }
		auto* vertices = Util::AsReal(mesh->vertexBuffer);
		auto* indices = Util::AsReal(mesh->indexBuffer);
		D3D11_BUFFER_DESC vb{}, ib{}; vertices->GetDesc(&vb); indices->GetDesc(&ib);
		const auto descriptor = std::bit_cast<uint64_t>(data.vertexDesc);
		const auto stride = uint32_t(descriptor & 0xf) * sizeof(float);
		const uint32_t count = uint32_t(shape->GetTrishapeRuntimeData().triangleCount) * 3;
		// SSE stores float positions even with VF_FULLPREC clear, so that flag cannot select decoding.
		bool supportedPositionLayout = stride >= kSSEPositionBytes;
		const auto flags = static_cast<uint32_t>(data.vertexDesc.GetFlags());
		for (uint32_t attribute = 1; attribute < RE::BSGraphics::Vertex::VA_COUNT; ++attribute) {
			if (!(flags & (1u << attribute))) continue;
			const auto offset = data.vertexDesc.GetAttributeOffset(static_cast<RE::BSGraphics::Vertex::Attribute>(attribute));
			supportedPositionLayout &= offset >= kSSEPositionBytes && offset < stride;
		}
		if (!supportedPositionLayout) { Reject(Rejection::UnsupportedPositionLayout); return; }
		if (vb.Usage != D3D11_USAGE_IMMUTABLE) { Reject(Rejection::VertexBufferUsage); return; }
		if (ib.Usage != D3D11_USAGE_IMMUTABLE) { Reject(Rejection::IndexBufferUsage); return; }
		if (stride < sizeof(float) * 3 || !vb.ByteWidth || vb.ByteWidth % stride || ib.ByteWidth % sizeof(uint16_t)) { Reject(Rejection::BufferLayout); return; }
		if (!count || uint64_t(count) * sizeof(uint16_t) > ib.ByteWidth) { Reject(Rejection::IndexRange); return; }
		auto& record = batch.records[batch.count];
		alignas(16) DirectX::XMFLOAT3X4 world;
		Util::StoreTransform3x4NoScale(world, a_geometry.world);
		bool finite = std::isfinite(a_geometry.world.scale) && a_geometry.world.scale != 0;
		for (auto& row : world.m) {
			for (uint32_t i = 0; i < 3; ++i) row[i] *= a_geometry.world.scale;
			for (float value : row) finite &= std::isfinite(value);
		}
		const auto& bound = a_geometry.worldBound;
		for (float value : { bound.center.x, bound.center.y, bound.center.z, bound.radius }) finite &= std::isfinite(value);
		if (!finite || bound.radius < 0) { Reject(Rejection::NonFiniteTransformOrBound); return; }
		record.vertices.copy_from(vertices); record.indices.copy_from(indices);
		record.world = world; record.bound = { bound.center.x, bound.center.y, bound.center.z, bound.radius };
		record.descriptor = descriptor; record.first = 0; record.count = count;
		record.twoSided = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
		++batch.count;
		if (writing->countDiagnostics && !data.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC)) ++writing->admittedWithoutFullPrecisionFlag;
	}
	void End()
	{
		if (!writing) { Count(Event::EndWithoutCapture); return; }
		Count(Event::CaptureEnds);
		if (writing->batch.scope.scene != epoch.load()) { Count(Event::CaptureEpochMismatch); writing->batch.incomplete = true; }
		if (!writing->batch.scope.light) { Count(Event::MissingLightIdentity); writing->batch.incomplete = true; }
		writing->batch.incomplete |= writing->batch.omissions != 0;
		if (writing->countDiagnostics) {
			eventCounts[static_cast<size_t>(Event::CallbacksInCapture)].fetch_add(writing->callbacks, std::memory_order_relaxed);
			eventCounts[static_cast<size_t>(Event::OverflowCallbacks)].fetch_add(writing->overflowCallbacks, std::memory_order_relaxed);
			eventCounts[static_cast<size_t>(Event::Duplicates)].fetch_add(writing->batch.duplicates, std::memory_order_relaxed);
			eventCounts[static_cast<size_t>(Event::Admitted)].fetch_add(writing->batch.count, std::memory_order_relaxed);
			eventCounts[static_cast<size_t>(Event::AdmittedWithoutFullPrecisionFlag)].fetch_add(writing->admittedWithoutFullPrecisionFlag, std::memory_order_relaxed);
			for (size_t i = 0; i < writing->rejections.size(); ++i)
				if (writing->rejections[i]) rejectionCounts[i].fetch_add(writing->rejections[i], std::memory_order_relaxed);
		}
		writing->state.store(SlotState::Ready, std::memory_order_release);
		writing = nullptr; ++completed;
	}
	Batch* Acquire()
	{
		Slot* oldest = nullptr;
		for (auto& slot : slots) if (slot.state.load(std::memory_order_acquire) == SlotState::Ready && (!oldest || slot.batch.scope.batch < oldest->batch.scope.batch)) oldest = &slot;
		if (!oldest) return nullptr;
		auto ready = SlotState::Ready;
		return oldest->state.compare_exchange_strong(ready, SlotState::Reading) ? &oldest->batch : nullptr;
	}
	void Release(Batch* a_batch)
	{
		for (auto& slot : slots) if (&slot.batch == a_batch) {
			for (uint32_t i = 0; i < slot.batch.count; ++i) { slot.batch.records[i].vertices = nullptr; slot.batch.records[i].indices = nullptr; }
			slot.state.store(SlotState::Free, std::memory_order_release); return;
		}
	}
	json Diagnostics()
	{
		json events = json::object(), rejections = json::object();
		for (size_t i = 0; i < kEventNames.size(); ++i) events[kEventNames[i]] = eventCounts[i].load(std::memory_order_relaxed);
		for (size_t i = 0; i < kRejectionNames.size(); ++i) rejections[kRejectionNames[i]] = rejectionCounts[i].load(std::memory_order_relaxed);
		return {
			{ "diagnosticCountersEnabled", diagnosticsEnabled.load(std::memory_order_relaxed) },
			{ "events", std::move(events) }, { "admissionRejections", std::move(rejections) },
			{ "enabled", enabled.load() }, { "selectedFrame", selectedFrame.load() }, { "selectedSlot", selectedSlot.load() },
			{ "consumerFrame", consumerFrame.load() },
			{ "attemptedFrame", attemptedFrame.load() }, { "missingObservation", selectedFrame.load() != attemptedFrame.load() },
			{ "sceneGeneration", epoch.load() }, { "completed", completed.load() }, { "dropped", dropped.load() },
			{ "recordCapacity", kCapacity }, { "maximumObservations", kMaximumObservations }, { "coverageValid", false }
		};
	}
}

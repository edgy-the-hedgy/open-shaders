#include "SurfaceFramePublisher.h"

#include "Features/DX12NativeScene/VideoMemoryBudget.h"
#include "GpuPass.h"
#include "GraphicsInterop.h"
#include "PCH.h"

#include <algorithm>
#include <format>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace
{
	struct RetiredSurfaceSlots
	{
		struct Entry
		{
			std::shared_ptr<void> owner;
			std::function<bool()> complete;
		};
		std::mutex mutex;
		std::vector<Entry> entries;

		static RetiredSurfaceSlots& Global()
		{
			static auto* result = new RetiredSurfaceSlots;
			return *result;
		}
		void Collect()
		{
			std::scoped_lock lock(mutex);
			std::erase_if(entries, [](const Entry& entry) { return entry.complete(); });
		}
		void Retain(std::shared_ptr<void> a_owner, std::function<bool()> a_complete)
		{
			std::scoped_lock lock(mutex);
			entries.push_back({ std::move(a_owner), std::move(a_complete) });
		}
	};

	uint64_t TextureBytes(ID3D12Device* a_device, const D3D11_TEXTURE2D_DESC& a_source)
	{
		D3D12_RESOURCE_DESC description{};
		description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		description.Width = a_source.Width;
		description.Height = a_source.Height;
		description.DepthOrArraySize = description.MipLevels = 1;
		description.Format = a_source.Format;
		description.SampleDesc.Count = 1;
		const auto bytes = a_device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
		if (bytes == UINT64_MAX)
			throw std::runtime_error("NativePBR surface format has no DX12 allocation size");
		return bytes;
	}

	bool ValidSource(ID3D11Texture2D* a_texture, DXGI_FORMAT a_format, uint32_t a_width, uint32_t a_height,
		D3D11_TEXTURE2D_DESC& a_description)
	{
		if (!a_texture)
			return false;
		a_texture->GetDesc(&a_description);
		return a_description.Format == a_format && a_description.Width && a_description.Height &&
			(!a_width || (a_description.Width == a_width && a_description.Height == a_height)) &&
			a_description.ArraySize == 1 && a_description.MipLevels == 1 && a_description.SampleDesc.Count == 1 &&
			a_description.Usage == D3D11_USAGE_DEFAULT;
	}
}

namespace NativePBR
{
	struct SurfaceFramePublisher::State
	{
		struct Slot
		{
			std::array<std::unique_ptr<WrappedResource>, kSurfaceFrameAttachmentCount> textures;
			std::array<DXGI_FORMAT, kSurfaceFrameAttachmentCount> formats{};
			winrt::com_ptr<ID3D12Device> device;
			winrt::com_ptr<ID3D12Fence> producerFence;
			std::weak_ptr<const SurfaceFrame> published;
			std::vector<std::pair<winrt::com_ptr<ID3D12Fence>, uint64_t>> consumers;
			std::mutex mutex;
			uint64_t bytes = 0, readyValue = 0, serial = 0;
			uint32_t width = 0, height = 0, capacity = 0;

			bool Reusable()
			{
				std::scoped_lock lock(mutex);
				if (!published.expired() || readyValue == UINT64_MAX)
					return false;
				if (readyValue) {
					if (!producerFence)
						return false;
					const auto completed = producerFence->GetCompletedValue();
					if (completed == UINT64_MAX || completed < readyValue)
						return false;
				}
				return std::ranges::all_of(consumers, [](const auto& consumer) {
					const auto completed = consumer.first->GetCompletedValue();
					return completed != UINT64_MAX && completed >= consumer.second;
				});
			}

			bool Retain(ID3D12Fence* a_fence, uint64_t a_value, uint64_t a_serial)
			{
				if (!a_fence || !a_value || a_value == UINT64_MAX)
					return false;
				winrt::com_ptr<ID3D12Device> owner;
				if (FAILED(a_fence->GetDevice(IID_PPV_ARGS(owner.put()))) || owner.get() != device.get())
					return false;
				std::scoped_lock lock(mutex);
				if (serial != a_serial || published.expired())
					return false;
				winrt::com_ptr<ID3D12Fence> fence;
				fence.copy_from(a_fence);
				consumers.emplace_back(std::move(fence), a_value);
				return true;
			}

			bool Matches(uint32_t a_width, uint32_t a_height, const std::array<D3D11_TEXTURE2D_DESC,
				kSurfaceFrameAttachmentCount>& a_sources, uint32_t a_count) const
			{
				if (width != a_width || height != a_height || capacity < a_count)
					return false;
				for (uint32_t index = 0; index < a_count; ++index)
					if (!textures[index] || formats[index] != a_sources[index].Format)
						return false;
				return true;
			}

			static std::shared_ptr<Slot> Allocate(ID3D11Device5* a_device11, ID3D12Device* a_device12,
				ID3D12Fence* a_producerFence, const std::array<D3D11_TEXTURE2D_DESC,
				kSurfaceFrameAttachmentCount>& a_descriptions, uint32_t a_count)
			{
				auto result = std::make_shared<Slot>();
				result->device.copy_from(a_device12);
				result->producerFence.copy_from(a_producerFence);
				result->width = a_descriptions[0].Width;
				result->height = a_descriptions[0].Height;
				result->capacity = a_count;
				constexpr std::array names{ "IdentityColor", "UvDdx", "DdyDepthFacing", "Basis0", "Basis1", "Basis2", "ProjectedWorld", "ProjectedDirection" };
				for (uint32_t index = 0; index < a_count; ++index) {
					const auto bytes = TextureBytes(a_device12, a_descriptions[index]);
					if (result->bytes > UINT64_MAX - bytes)
						throw std::runtime_error("NativePBR surface allocation accounting overflow");
					result->bytes += bytes;
					auto desc = a_descriptions[index];
					desc.Usage = D3D11_USAGE_DEFAULT;
					desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
					desc.CPUAccessFlags = desc.MiscFlags = 0;
					result->textures[index] = std::make_unique<WrappedResource>(desc, a_device11, a_device12,
						std::format("NativePBR::Surface{}", names[index]));
					result->formats[index] = desc.Format;
				}
				return result;
			}
		};

		std::array<std::shared_ptr<Slot>, kSurfaceFrameSlotCount> slots;
		std::shared_ptr<const SurfaceFrame> current;
		winrt::com_ptr<ID3D11Device5> device11;
		winrt::com_ptr<ID3D12Device> device12;
		winrt::com_ptr<ID3D12CommandQueue> queue12;
		SharedFence producer;
		DX12Native::VideoMemoryBudget budget;
		Diagnostics diagnostics;
		mutable std::mutex mutex;

		~State()
		{
			current.reset();
			for (auto& slot : slots)
				if (slot && !slot->Reusable()) {
					std::weak_ptr<Slot> weak = slot;
					RetiredSurfaceSlots::Global().Retain(slot, [weak] {
						const auto alive = weak.lock();
						return !alive || alive->Reusable();
					});
				}
		}
	};

	SurfaceFramePublisher::SurfaceFramePublisher() : state(std::make_unique<State>()) {}
	SurfaceFramePublisher::~SurfaceFramePublisher() = default;

	bool SurfaceFramePublisher::Publish(ID3D11Device5* a_device11, ID3D11DeviceContext4* a_context11,
		ID3D12Device* a_device12, ID3D12CommandQueue* a_queue12, const Sources& a_sources,
		uint64_t a_optionalBudgetBytes)
	{
		CS_GPU_PASS("NativePBR::PublishSurfaceFrame");
		RetiredSurfaceSlots::Global().Collect();
		std::scoped_lock lock(state->mutex);
		auto& stats = state->diagnostics;
		state->current.reset();
		try {
			if (!a_device11 || !a_context11 || !a_device12 || !a_queue12)
				throw std::runtime_error("NativePBR DX11/DX12 transfer backend unavailable");
			winrt::com_ptr<ID3D11Device> contextDevice;
			a_context11->GetDevice(contextDevice.put());
			winrt::com_ptr<ID3D12Device> queueDevice;
			if (contextDevice.get() != static_cast<ID3D11Device*>(a_device11) ||
				FAILED(a_queue12->GetDevice(IID_PPV_ARGS(queueDevice.put()))) || queueDevice.get() != a_device12)
				throw std::runtime_error("NativePBR DX11 context or DX12 queue belongs to another device");
			if (a_sources.abiVersion != kSurfaceFrameAbiVersion || !a_sources.nativeOwnedDraws ||
				!a_sources.draws || !ValidSurfaceDrawTable(*a_sources.draws)) {
				++stats.invalidDrawTables;
				throw std::runtime_error("NativePBR ABI-v2 draw ownership or immutable draw table unavailable");
			}
			if (!state->producer.fence12) {
				SharedFence producer;
				producer.Create(a_device12, a_device11, "NativePBR::SurfaceReady");
				state->device11.copy_from(a_device11);
				state->device12.copy_from(a_device12);
				state->queue12.copy_from(a_queue12);
				state->producer = std::move(producer);
			}
			if (state->device11.get() != a_device11 || state->device12.get() != a_device12 ||
				state->queue12.get() != a_queue12)
				throw std::runtime_error("NativePBR transfer backend changed; create a new publisher");
			std::array<D3D11_TEXTURE2D_DESC, kSurfaceFrameAttachmentCount> descriptions{};
			for (uint32_t index = 0; index < kSurfaceFrameBaseAttachments; ++index)
				if (!ValidSource(a_sources.textures[index], kSurfaceFrameFormats[index],
					index ? descriptions[0].Width : 0, index ? descriptions[0].Height : 0, descriptions[index])) {
					++stats.unavailableSources;
					throw std::runtime_error(std::format("NativePBR ABI-v2 surface attachment {} missing or format/dimensions incompatible", index));
				}
			for (uint32_t index = 0; index < kSurfaceFrameBaseAttachments; ++index) {
				winrt::com_ptr<ID3D11Device> owner;
				a_sources.textures[index]->GetDevice(owner.put());
				if (owner.get() != contextDevice.get())
					throw std::runtime_error("NativePBR surface source belongs to another DX11 device");
			}
			const uint32_t width = descriptions[0].Width, height = descriptions[0].Height;
			const bool projectedRequested = std::ranges::any_of(*a_sources.draws, [](const SidecarDraw& draw) {
				return draw.projectedUv;
			});
			bool projectedReady = projectedRequested;
			for (uint32_t index = kSurfaceFrameBaseAttachments; index < kSurfaceFrameAttachmentCount; ++index)
				if (projectedRequested)
					projectedReady &= ValidSource(a_sources.textures[index], kSurfaceFrameFormats[index], width, height, descriptions[index]);
			if (projectedReady)
				for (uint32_t index = kSurfaceFrameBaseAttachments; index < kSurfaceFrameAttachmentCount; ++index) {
					winrt::com_ptr<ID3D11Device> owner;
					a_sources.textures[index]->GetDevice(owner.put());
					projectedReady &= owner.get() == contextDevice.get();
				}
			uint32_t count = projectedReady ? kSurfaceFrameAttachmentCount : kSurfaceFrameBaseAttachments;
			auto& slots = state->slots;
			auto available = std::find_if(slots.begin(), slots.end(), [&](const auto& slot) {
				return slot && slot->Reusable() && slot->Matches(width, height, descriptions, count);
			});
			if (available == slots.end())
				available = std::find_if(slots.begin(), slots.end(), [](const auto& slot) { return !slot || slot->Reusable(); });
			if (available == slots.end()) {
				++stats.busySlots;
				throw std::runtime_error("NativePBR surface frame slots still owned by GPU consumers");
			}
			uint64_t trackedBytes = 0, otherBytes = 0;
			for (const auto& slot : slots)
				if (slot) {
					if (trackedBytes > UINT64_MAX - slot->bytes)
						throw std::runtime_error("NativePBR surface frame pool accounting overflow");
					trackedBytes += slot->bytes;
					if (&slot != &*available)
						otherBytes += slot->bytes;
				}
			const auto budget = state->budget.Query(a_device12, trackedBytes, a_optionalBudgetBytes);
			stats.dxgiAvailable = budget.dxgiAvailable;
			stats.effectiveBudgetBytes = budget.effectiveBytes;
			stats.localBudgetBytes = budget.localBudget;
			stats.localUsageBytes = budget.localUsage;
			if (!budget.dxgiAvailable)
				throw std::runtime_error("NativePBR DXGI local-memory budget unavailable");
			auto requiredBytes = [&](uint32_t a_count, bool a_newAllocation = false) {
				if (!a_newAllocation && *available && (*available)->Matches(width, height, descriptions, a_count))
					return (*available)->bytes;
				uint64_t bytes = 0;
				for (uint32_t index = 0; index < a_count; ++index) {
					const auto additional = TextureBytes(a_device12, descriptions[index]);
					if (bytes > UINT64_MAX - additional)
						throw std::runtime_error("NativePBR surface allocation size overflow");
					bytes += additional;
				}
				return bytes;
			};
			bool trimProjected = false;
			if (otherBytes > budget.effectiveBytes || requiredBytes(count) > budget.effectiveBytes - otherBytes) {
				if (count == kSurfaceFrameAttachmentCount)
					count = kSurfaceFrameBaseAttachments;
				if (otherBytes <= budget.effectiveBytes && requiredBytes(count) > budget.effectiveBytes - otherBytes)
					trimProjected = requiredBytes(count, true) <= budget.effectiveBytes - otherBytes;
				if (otherBytes > budget.effectiveBytes ||
					(!trimProjected && requiredBytes(count) > budget.effectiveBytes - otherBytes)) {
					++stats.budgetDeferrals;
					throw std::runtime_error("NativePBR surface frame deferred by dynamic DXGI budget");
				}
			}
			auto& selected = *available;
			if (!selected || trimProjected || !selected->Matches(width, height, descriptions, count)) {
				// Reusing old allocation credit requires releasing the idle slot before allocating its replacement.
				selected.reset();
				try {
					selected = State::Slot::Allocate(a_device11, a_device12, state->producer.fence12.get(), descriptions, count);
				} catch (const std::exception&) {
					++stats.allocationFailures;
					if (count != kSurfaceFrameAttachmentCount)
						throw;
					count = kSurfaceFrameBaseAttachments;
					if (otherBytes > budget.effectiveBytes ||
						requiredBytes(count, true) > budget.effectiveBytes - otherBytes)
						throw;
					selected = State::Slot::Allocate(a_device11, a_device12, state->producer.fence12.get(), descriptions, count);
				}
			}
			const bool publishProjected = projectedReady && count == kSurfaceFrameAttachmentCount;
			auto frame = std::make_shared<SurfaceFrame>();
			frame->frame = a_sources.frame;
			frame->width = width;
			frame->height = height;
			frame->nativeOwnedDraws = true;
			frame->projectedAvailable = publishProjected;
			frame->draws = a_sources.draws;
			frame->readyFence = state->producer.fence12;
			for (uint32_t index = 0; index < count; ++index) {
				frame->textures[index] = selected->textures[index]->resource;
				frame->states[index] = D3D12_RESOURCE_STATE_COMMON;
			}
			{
				std::scoped_lock slotLock(selected->mutex);
				selected->readyValue = UINT64_MAX;
				selected->consumers.clear();
				++selected->serial;
			}
			const auto serial = selected->serial;
			frame->retainThrough = [owner = selected, serial](ID3D12Fence* a_fence, uint64_t a_value) {
				return owner->Retain(a_fence, a_value, serial);
			};
			for (uint32_t index = 0; index < count; ++index)
				a_context11->CopyResource(selected->textures[index]->resource11, a_sources.textures[index]);
			const auto ready = state->producer.Next();
			DX::ThrowIfFailed(a_context11->Signal(state->producer.fence11.get(), ready));
			a_context11->Flush();
			selected->readyValue = ready;
			frame->readyValue = ready;
			selected->published = frame;
			state->current = std::move(frame);
			++stats.published;
			if (publishProjected)
				++stats.projectedPublished;
			else if (projectedRequested)
				++stats.projectedDeferred;
			stats.status = !publishProjected && projectedRequested ?
				"NativePBR opaque surface published; optional projected inputs deferred" :
				"NativePBR ABI-v2 opaque surface published";
			stats.allocatedBytes = otherBytes + selected->bytes;
			return true;
		} catch (const std::exception& error) {
			stats.status = error.what();
			return false;
		}
	}

	std::shared_ptr<const SurfaceFrame> SurfaceFramePublisher::Get(uint32_t a_frame) const
	{
		std::scoped_lock lock(state->mutex);
		return state->current && state->current->frame == a_frame ? state->current : nullptr;
	}

	bool SurfaceFramePublisher::RetainThrough(const std::shared_ptr<const SurfaceFrame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value)
	{
		return a_frame && a_frame->retainThrough && a_frame->retainThrough(a_fence, a_value);
	}

	const SurfaceFramePublisher::Diagnostics& SurfaceFramePublisher::GetDiagnostics() const { return state->diagnostics; }
}

#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>

namespace Util
{
	enum class RuntimePreparationLane : uint8_t { General, Geometry, Character, Rigid, Material, Light, Count };
	inline constexpr std::size_t kRuntimePreparationLaneCount = static_cast<std::size_t>(RuntimePreparationLane::Count);
	// Separate admission quotas prevent a burst of new rigid meshes from
	// using the slots needed by the other preparation producers.
	inline constexpr std::array<uint32_t, kRuntimePreparationLaneCount> kRuntimePreparationLaneLimits{ 3, 2, 2, 4, 3, 2 };
	constexpr bool ValidRuntimePreparationLane(RuntimePreparationLane lane)
	{
		return static_cast<std::size_t>(lane) < kRuntimePreparationLaneCount;
	}
	constexpr bool RuntimePreparationLaneHasCapacity(RuntimePreparationLane lane, uint32_t queued)
	{
		return ValidRuntimePreparationLane(lane) && queued < kRuntimePreparationLaneLimits[static_cast<std::size_t>(lane)];
	}

	/** @brief Lifetime-safe access to the existing runtime task dispatcher.
	 * Fence callbacks may outlive the feature, but cannot call a closed dispatcher. */
	class RuntimeTaskGate
	{
	public:
		using Task = std::function<void()>;
		using Submitter = std::function<bool(Task)>;
		using TaggedSubmitter = std::function<bool(RuntimePreparationLane, Task)>;
		using Reserver = std::function<uint64_t(RuntimePreparationLane)>;
		using Completer = std::function<bool(uint64_t, Task)>;

		explicit RuntimeTaskGate(Submitter submit) :
			submitter([submit = std::move(submit)](RuntimePreparationLane, Task task) { return submit && submit(std::move(task)); }) {}
		explicit RuntimeTaskGate(TaggedSubmitter submit, Reserver reserve = {}, Completer complete = {}) :
			submitter(std::move(submit)), reserver(std::move(reserve)), completer(std::move(complete)) {}

		// A placeholder counts against admission quotas but is not dispatchable.
		// Acquisition occurs outside all scheduler locks. Complete with an empty
		// task to cancel, including on exception or a rejected engine snapshot.
		uint64_t TryReserve(RuntimePreparationLane lane)
		{
			std::scoped_lock lock(mutex);
			return reserver && ValidRuntimePreparationLane(lane) ? reserver(lane) : 0;
		}
		bool CompleteReservation(uint64_t id, Task task = {})
		{
			std::scoped_lock lock(mutex);
			return id && completer && completer(id, std::move(task));
		}

		bool TrySubmit(Task task) { return TrySubmit(RuntimePreparationLane::General, std::move(task)); }
		bool TrySubmit(RuntimePreparationLane lane, Task task)
		{
			std::scoped_lock lock(mutex);
			return submitter && task && ValidRuntimePreparationLane(lane) && submitter(lane, std::move(task));
		}

		void Close()
		{
			std::scoped_lock lock(mutex);
			submitter = {};
			reserver = {};
			completer = {};
		}

	private:
		std::mutex mutex;
		TaggedSubmitter submitter;
		Reserver reserver;
		Completer completer;
	};

	// Legacy immediate admission predicate, retained for policy tests and clients.
	// Native preparation uses the bounded FIFO policy below.
	inline constexpr uint32_t kRuntimePreparationQueueLimit = 16;
	inline constexpr uint32_t kRuntimePreparationConcurrency = 3;

	// Demand shaders normally win. After a bounded dispatch burst, permit
	// one native job (without exceeding the shared concurrency budget).
	inline constexpr uint32_t kShaderDispatchBurst = 16;
	constexpr bool RuntimePreparationFairTurn(uint32_t shaderDispatches)
	{
		return shaderDispatches >= kShaderDispatchBurst;
	}
	// Reserve a shader slot where the pool has more than one slot.
	constexpr bool CanDispatchRuntimePreparation(uint32_t running, uint32_t inFlight,
		int32_t limit, bool demanded, bool backgroundPending, bool fairTurn = false)
	{
		if ((demanded && !fairTurn) || limit <= 0 || running >= kRuntimePreparationConcurrency ||
			inFlight >= static_cast<uint32_t>(limit)) return false;
		if (demanded || backgroundPending)
			return limit == 1 ? fairTurn : running < static_cast<uint32_t>(limit - 1);
		return true;
	}

	constexpr bool CanQueueRuntimePreparation(bool closed, uint32_t queued)
	{
		return !closed && queued < kRuntimePreparationQueueLimit;
	}

	constexpr bool CanAdmitRuntimeTask(bool closed, bool runtimeInFlight, bool demandedShaders,
		uint32_t inFlight, int32_t limit)
	{
		return !closed && !runtimeInFlight && !demandedShaders && limit > 0 &&
		       inFlight < static_cast<uint32_t>(limit);
	}
}

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

		explicit RuntimeTaskGate(Submitter submit) :
			submitter([submit = std::move(submit)](RuntimePreparationLane, Task task) { return submit && submit(std::move(task)); }) {}
		explicit RuntimeTaskGate(TaggedSubmitter submit) : submitter(std::move(submit)) {}

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
		}

	private:
		std::mutex mutex;
		TaggedSubmitter submitter;
	};

	// Legacy immediate admission predicate, retained for policy tests and clients.
	// Native preparation uses the bounded FIFO policy below.
	inline constexpr uint32_t kRuntimePreparationQueueLimit = 16;
	inline constexpr uint32_t kRuntimePreparationConcurrency = 3;

	// Demand shaders always win. Reserve one slot for compilation when there
	// is background shader work; never exceed the shared pool's current limit.
	constexpr bool CanDispatchRuntimePreparation(uint32_t running, uint32_t inFlight,
		int32_t limit, bool demanded, bool backgroundPending)
	{
		if (demanded || limit <= 0 || running >= kRuntimePreparationConcurrency ||
			inFlight >= static_cast<uint32_t>(limit)) return false;
		return !backgroundPending || (limit > 1 && running < static_cast<uint32_t>(limit - 1));
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

#pragma once

#include <array>
<<<<<<< HEAD
#include <atomic>
#include <cstdint>
||||||| 12b261724
=======
#include <atomic>
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
#include <d3d11.h>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <winrt/base.h>

#include "Utils/GpuTimestamps.h"
#include "Utils/Macros.h"

/**
 * @brief GPU and CPU profiler using D3D11 timestamp queries.
 *
 * Maintains a GPU query ring and frame-scoped CPU samples
 * with rolling statistics (average, p95, p99) per named pass. Capture is
 * request-driven: no queries or QPC reads are issued unless a capture is
 * active for the current frame.
 */
class Profiler
{
public:
<<<<<<< HEAD
	static constexpr uint32_t kMaxTimers = 1024;
	static constexpr uint32_t kFrameLatency = 3;
||||||| 12b261724
	static constexpr uint32_t kMaxTimers = 128;
	static constexpr uint32_t kFrameLatency = 3;
=======
	static constexpr uint32_t kMaxTimers = 128;
	static constexpr uint32_t kFrameLatency = 6;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	static constexpr uint32_t kHistorySize = 300;
	/** @brief Collection-cycle gap before retiring an inactive timer.
		Must exceed the longest legitimate gap between samples of a still-running pass;
		the DynamicCubemaps state machine spreads its passes over 6 frames. */
	static constexpr uint64_t kTimerRetireFrames = 60;

	using PerfEventCallback = std::function<void(std::string_view)>;

	enum class CaptureMode : uint8_t
	{
		None = 0,
		GPU = 1,
		CPU = 2,
		Both = 3
	};

	/** @brief Circular buffer tracking per-timer timing samples with statistics. */
	struct RollingHistory
	{
		float history[kHistorySize]{};
		uint32_t head = 0;
		uint32_t count = 0;
		float lastMs = 0.0f;

		/** @brief Appends a timing sample, overwriting the oldest if full. */
		void PushSample(float ms)
		{
			history[head] = ms;
			head = (head + 1) % kHistorySize;
			if (count < kHistorySize)
				count++;
			lastMs = ms;
			sortedDirty = true;
		}

		/** @brief Gets the arithmetic mean of all buffered samples. */
		float GetAverage() const;

		/**
		 * @brief Gets an interpolated percentile from the buffered samples.
		 * @param p Percentile in [0, 100].
		 */
		float GetPercentile(float p) const;

	private:
		// Reuse one sorted snapshot for every percentile until the next sample arrives.
		mutable std::array<float, kHistorySize> sorted{};
		mutable bool sortedDirty = true;
	};

	/** @brief Snapshot of GPU/CPU timing data for a single named pass. */
	struct TimerResult
	{
		std::string name;
		/// GPU self time, excluding all directly nested profiled passes.
		float gpuTimeMs = 0.0f;
		/// Inclusive duration contributed by depth-0 intervals this cycle.
		/// Zero when not active this cycle; sums exactly to the frame total.
		float topLevelMs = 0.0f;
		// Rolling statistics, filled in only by GetStatistics.
		float avgMs = 0.0f;
		float p95Ms = 0.0f;
		float p99Ms = 0.0f;
		float cpuTimeMs = 0.0f;
		float cpuAvgMs = 0.0f;
		float cpuP95Ms = 0.0f;
		float cpuP99Ms = 0.0f;
		/// Whether this pass has ever recorded a GPU/CPU sample, vs. one
		/// mode never having applied to it (a CPU-only scope has no GPU side).
		bool hasGpu = false;
		bool hasCpu = false;
		/// Whether the latest collected frame for this side had a fresh sample
		/// (vs. the stats reflecting an older, stale sample).
		bool activeGpu = false;
		bool activeCpu = false;
		bool valid = false;

		const float* historyBuffer = nullptr;
		uint32_t historyHead = 0;
		uint32_t historyCount = 0;
		const float* cpuHistoryBuffer = nullptr;
		uint32_t cpuHistoryHead = 0;
		uint32_t cpuHistoryCount = 0;

		/**
		 * @brief Gets a GPU history sample by age-ordered index (0 = oldest).
		 * @param index Zero-based index into the history ring buffer.
		 */
		float GetHistorySample(uint32_t index) const
		{
			if (!historyBuffer || index >= historyCount)
				return 0.0f;
			return historyBuffer[(historyHead - historyCount + index + kHistorySize) % kHistorySize];
		}

		/**
		 * @brief Gets a CPU history sample by age-ordered index (0 = oldest).
		 * @param index Zero-based index into the history ring buffer.
		 */
		float GetCpuHistorySample(uint32_t index) const
		{
			if (!cpuHistoryBuffer || index >= cpuHistoryCount)
				return 0.0f;
			return cpuHistoryBuffer[(cpuHistoryHead - cpuHistoryCount + index + kHistorySize) % kHistorySize];
		}
	};

	/** @brief Rolling wall-clock statistics of the interval between EndFrame calls. */
	struct FrameStats
	{
		float lastMs = 0.0f;
		float avgMs = 0.0f;
		float p95Ms = 0.0f;
		float p99Ms = 0.0f;
		uint32_t count = 0;
	};

	/**
	 * @brief Creates timestamp query objects and prepares the frame ring buffer.
	 * @param device D3D11 device used to create query objects.
	 * @param context Device context used for issuing and collecting queries.
	 */
	void Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

	/** @brief Releases all D3D11 query objects and resets state. */
	void Release();

	/** @brief Enables or disables runtime profiling; disabling cancels any pending capture. */
	void SetUserEnabled(bool a_enabled);

	/** @brief Gets whether the user has runtime profiling enabled. */
	bool IsUserEnabled() const { return userEnabled.load(std::memory_order_acquire); }

	/**
	 * @brief Requests a timing capture for the next frame; consumers must re-request every frame.
	 * @param a_mode Timing sources to acquire.
	 * @param a_namePrefix Optional pass-name prefix applied before timer acquisition.
	 */
	void RequestCapture(CaptureMode a_mode = CaptureMode::Both, std::string_view a_namePrefix = {});

	/** @brief True while a capture is active; gates all query issuance and CPU timing. */
	bool IsEnabled() const { return IsUserEnabled() && captureActive.load(std::memory_order_acquire); }

	/**
	 * @brief Registers optional callbacks invoked at pass begin/end (e.g. for RenderDoc markers).
	 * @param beginCb Called with the pass name when a pass begins.
	 * @param endCb Called when a pass ends.
	 */
	void SetPerfEventCallbacks(PerfEventCallback beginCb, PerfEventCallback endCb)
	{
		beginPerfEvent = std::move(beginCb);
		endPerfEvent = std::move(endCb);
	}

<<<<<<< HEAD
	/**
	 * @brief Begins a new profiling frame; collects results from the oldest in-flight frame.
	 * If those are still pending or the query batch cannot begin, the whole frame is skipped
	 * (no GPU sample, no capture cycle) until EndFrame; CPU timing still runs.
	 */
||||||| 12b261724
	/** @brief Begins a new profiling frame; collects results from the oldest in-flight frame. */
=======
	/** @brief Collects resolved frames; begins one unless the ring is full. */
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	void BeginFrame();

	/**
	 * @brief Opens a named pass; returns false (no-op EndPass expected) at
	 * capacity or before Initialize(). Passes may nest: each call acquires
	 * its own slot, and EndPass closes the innermost still-open one.
	 */
<<<<<<< HEAD
	bool BeginPass(std::string_view name, bool fireCallbacks = true);
	void EndPass(bool fireCallbacks = true);
||||||| 12b261724
	void BeginPass(const std::string& name);
=======
	void BeginPass(std::string_view name);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

	/**
	 * @brief Ends the current profiling frame.
	 * @param a_frameCount The engine's own frame counter for this tick,
	 *        stamped onto each active capture. CPU results publish at this
	 *        boundary; GPU results publish when their queries are ready.
	 */
	void EndFrame(uint32_t a_frameCount);

<<<<<<< HEAD
	/**
	 * @brief Opens a CPU-only named scope (no GPU query); returns false (no-op
	 * EndCpuPass expected) while disabled. Scopes may nest: each successful
	 * call pushes its own slot, and EndCpuPass closes the innermost open one.
	 */
	bool BeginCpuPass(std::string_view name);
	void EndCpuPass();
||||||| 12b261724
	/** @brief Ends the current profiling frame and advances the ring buffer write cursor. */
	void EndFrame();
=======
	/**
	 * @brief Records a zero sample for a pass skipped on purpose this frame, so its row reads 0 instead of its last cost.
	 * @param name Pass identifier, as given to BeginPass. Ignored if that pass began this frame or has no row yet.
	 * @note Needs an active profiler. A mark made before the frame's first BeginPass waits for it, and is dropped if no profiling frame begins before EndFrame. Zero samples do not keep a row alive, so a pass skipped for kTimerRetireFrames retires. Ignored on a frame that ran out of timers.
	 */
	void MarkPassSkipped(std::string_view name);

	/** @brief Ends the current profiling frame and advances the ring buffer write cursor. */
	void EndFrame();
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

	/**
	 * @brief Keeps GPU/CPU pass timing running for the next few frames; call it each frame a consumer reads the results.
	 * @param frameCount Frames to stay active for. Timing stops once no consumer has asked within that window.
	 * A shorter request never cuts a longer one short, so the overlay cannot end a DevBench window.
	 */
	void KeepActive(uint32_t frameCount = 2)
	{
		const uint64_t until = frameCounter.load(std::memory_order_relaxed) + frameCount;
		uint64_t current = activeUntilFrame.load(std::memory_order_relaxed);
		while (current < until && !activeUntilFrame.compare_exchange_weak(current, until, std::memory_order_relaxed)) {
		}
	}
	/** @brief True while a consumer asked for timings within its KeepActive window. */
	bool IsActive() const { return frameCounter.load(std::memory_order_relaxed) <= activeUntilFrame.load(std::memory_order_relaxed); }

	/** @brief Gets the live timings and history from the last collected frame. Rolling statistics are left at zero; see GetStatistics. */
	const std::vector<TimerResult>& GetResults() const { return results; }

	/**
	 * @brief Computes the rolling CPU and GPU statistics of every tracked pass.
	 * Re-sorts every history that gained samples since the last call, so callers should throttle it.
	 * Not synchronized: call it only on the thread that calls EndFrame, like GetResults.
	 * @param timerPrefix Includes only passes starting with this prefix, or all passes when empty.
	 */
	std::vector<TimerResult> GetStatistics(std::string_view timerPrefix = {}) const;

	/**
	 * @brief Gets the present-cadence frame interval statistics, tracked even while GPU timing is off.
	 * Re-sorts the history if it gained a sample, so callers should throttle it.
	 * Not synchronized: call it only on the thread that calls EndFrame, like GetResults.
	 */
	FrameStats GetFrameStats() const;

	/** @brief Gets the total GPU time in milliseconds for the last collected frame. */
	float GetTotalTimeMs() const { return totalTimeMs; }

	/** @brief Gets the total CPU time in milliseconds for the last collected frame. */
	float GetCpuTotalTimeMs() const { return cpuTotalTimeMs; }

	/**
	 * @brief Gets the GPU total for GetCapturedGpuFrameCount().
	 *
	 * Unlike GetTotalTimeMs(), this is never zeroed while idle, so it stays
	 * resolve-consistent with GetResults()'s timers for exact checks like
	 * totalMs == sum(topLevelMs); GetTotalTimeMs() is the live/UI value that
	 * intentionally zeroes so an overlay row doesn't show a stale sum.
	 */
	float GetResolvedTotalTimeMs() const { return resolvedTotalMs; }

	/** @brief Gets the CPU total for GetCapturedCpuFrameCount(). */
	float GetResolvedCpuTotalTimeMs() const { return resolvedCpuTotalMs; }

	/** @brief Gets the newest frame published by either timing source. */
	uint32_t GetCapturedFrameCount() const { return capturedFrameCount; }
	/** @brief Gets the engine frame count of the latest resolved GPU capture. */
	uint32_t GetCapturedGpuFrameCount() const { return capturedGpuFrameCount; }
	/** @brief Gets the engine frame count of the latest resolved CPU capture. */
	uint32_t GetCapturedCpuFrameCount() const { return capturedCpuFrameCount; }

	/** @brief Gets the GPU timer slots acquired in the most recently completed frame. */
	uint32_t GetAcquiredSlots() const { return acquiredSlots; }

	/** @brief Gets the session high-water mark of GPU timer slots acquired in one frame. */
	uint32_t GetPeakAcquiredSlots() const { return peakAcquiredSlots; }

	/** @brief Gets the cumulative count of BeginPass calls refused for slot capacity since Initialize(). */
	uint32_t GetSlotRefusals() const { return slotRefusals; }

	/** @brief Resets all timer history and results. */
	void ClearTimers()
	{
		results.clear();
		knownTimers.clear();
		knownTimerIndex.clear();
		captureCycleCount = 0;
		totalTimeMs = 0.0f;
		cpuTotalTimeMs = 0.0f;
		resolvedTotalMs = 0.0f;
		resolvedCpuTotalMs = 0.0f;
		capturedFrameCount = 0;
		capturedGpuFrameCount = 0;
		capturedCpuFrameCount = 0;
		activeCpuTimers.clear();
		completedCpuTimers.clear();
		// A cleared timer must not resurrect from a still-in-flight ring slot.
		ResetPendingFrames();
	}

	/**
	 * @brief Removes all timers whose names start with the given feature prefix.
	 * @param featureName Feature name; timers matching "featureName::*" are removed.
	 */
	void ClearTimersForFeature(const std::string& featureName)
	{
		std::string prefix = featureName + "::";
		std::erase_if(knownTimers, [&prefix](const KnownTimer& kt) {
			return kt.name.starts_with(prefix);
		});
		RebuildTimerIndex();
		std::erase_if(activeCpuTimers, [&prefix](const CpuTimer& ct) {
			return ct.name.starts_with(prefix);
		});
		std::erase_if(completedCpuTimers, [&prefix](const CompletedCpuTimer& ct) {
			return ct.name.starts_with(prefix);
		});
		// Discard queued GPU results so removed timers cannot be republished.
		ResetPendingFrames();
		RebuildResults();
	}

private:
<<<<<<< HEAD
	/// A CPU-only scope still open, awaiting EndCpuPass.
	struct CpuTimer
||||||| 12b261724
	struct FrameQueries
=======
	/** @brief Pass names marked skipped for one frame. The string slots are reused, so marking allocates only on first use. */
	struct SkipMarks
	{
		std::vector<std::string> names;
		uint32_t count = 0;

		void Add(std::string_view name);
	};

	struct FrameQueries
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	{
<<<<<<< HEAD
		std::string name;
		LARGE_INTEGER cpuBegin{};
		/// Nesting depth at acquisition, stamped once at Begin rather than
		/// derived from activeCpuTimers.size() at End: ClearTimersForFeature
		/// can erase a middle stack entry, which would desync a derived depth.
		uint32_t depth = 0;
||||||| 12b261724
		winrt::com_ptr<ID3D11Query> disjoint;
		struct TimerPair
		{
			winrt::com_ptr<ID3D11Query> begin;
			winrt::com_ptr<ID3D11Query> end;
			std::string name;
			LARGE_INTEGER cpuBegin{};
			float cpuMs = 0.0f;
		};
		std::vector<TimerPair> timers;
		uint32_t activeCount = 0;
		bool inFlight = false;
=======
		winrt::com_ptr<ID3D11Query> disjoint;
		struct TimerPair
		{
			winrt::com_ptr<ID3D11Query> begin;
			winrt::com_ptr<ID3D11Query> end;
			std::string name;
			LARGE_INTEGER cpuBegin{};
			float cpuMs = 0.0f;
		};
		std::vector<TimerPair> timers;
		uint32_t activeCount = 0;
		SkipMarks skipped;
		// A pass began past kMaxTimers, so a marked pass may have run untimed.
		bool overflowed = false;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	};

	/// A CPU-only scope that finished this cycle, pending collection.
	struct CompletedCpuTimer
	{
		std::string name;
		float cpuMs = 0.0f;
		/// Nesting depth at acquisition (0 = top-level).
		uint32_t depth = 0;
	};

	struct FrameQueries
	{
		Util::TimestampQueryBatch batch;
		struct TimerMeta
		{
			std::string name;
			LARGE_INTEGER cpuBegin{};
			/// Slot of the directly enclosing GPU pass, or -1 at frame depth 0.
			int32_t parentSlot = -1;
			uint32_t depth = 0;
			uint32_t cpuDepth = 0;
		};
		std::vector<TimerMeta> timers;
		uint32_t acquiredTimerCount = 0;
		std::vector<int> activeStack;
		/// Includes filtered capture cycles with no matching pass.
		bool captureCycle = false;
		bool inFlight = false;
		uint32_t capturedFrame = 0;
		uint64_t capturedCycle = 0;
		std::string namePrefix;
	};

	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;

	FrameQueries frames[kFrameLatency];
	uint32_t writeFrame = 0;
	uint32_t readFrame = 0;
<<<<<<< HEAD
||||||| 12b261724
	uint32_t framesSinceInit = 0;
=======
	// Ended frames awaiting collection, oldest at readFrame.
	uint32_t pendingFrames = 0;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	bool initialized = false;
	bool frameActive = false;
<<<<<<< HEAD
	/// Set when the frame could not open at its first pass (oldest slot pending or batch failed);
	/// a bracket opened by a later pass would record the earlier passes as 0 ms.
	bool frameRefused = false;
	// Enabled by default so profiling pages show data without an extra toggle;
	// zero-overhead idling still holds because captureActive stays false until requested.
	std::atomic_bool userEnabled{ true };
	std::atomic_bool captureRequested{ false };
	std::atomic_bool captureActive{ false };
	std::mutex captureRequestLock;
	CaptureMode requestedCaptureMode = CaptureMode::None;
	std::string requestedNamePrefix;
	CaptureMode activeCaptureMode = CaptureMode::None;
	std::string activeNamePrefix;
	std::vector<bool> activePassUsesGpu;
	double cpuTicksToMs = 0.0;
||||||| 12b261724
	double cpuTicksToMs = 0.0;
=======
	bool passOpen = false;
	// Ring was full at BeginFrame; the whole frame is unprofiled.
	bool frameSkipped = false;
	static double QpcTicksToMs();

	double cpuTicksToMs = QpcTicksToMs();
	std::atomic<uint64_t> frameCounter{ 0 };
	std::atomic<uint64_t> activeUntilFrame{ 0 };
	RollingHistory frameHistory;
	int64_t lastFrameTicks = 0;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

	PerfEventCallback beginPerfEvent;
	PerfEventCallback endPerfEvent;

	std::vector<TimerResult> results;

	struct KnownTimer
	{
		std::string name;
		RollingHistory gpu;
		RollingHistory cpu;
		bool hasGpu = false;
		bool hasCpu = false;
		bool activeGpu = false;
		bool activeCpu = false;
		float topLevelMs = 0.0f;
		uint64_t lastSampleCycle = 0;
	};
	std::vector<KnownTimer> knownTimers;
	std::unordered_map<std::string, size_t> knownTimerIndex;
<<<<<<< HEAD
	uint64_t captureCycleCount = 0;
||||||| 12b261724
	uint64_t collectedFrames = 0;
=======
	/** @brief Skip marks made before the frame's first BeginPass; they join that frame. */
	SkipMarks pendingSkipped;
	uint64_t collectedFrames = 0;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	float totalTimeMs = 0.0f;
	float cpuTotalTimeMs = 0.0f;
	float resolvedTotalMs = 0.0f;
	float resolvedCpuTotalMs = 0.0f;
	uint32_t capturedFrameCount = 0;
	uint32_t capturedGpuFrameCount = 0;
	uint32_t capturedCpuFrameCount = 0;
	uint32_t acquiredSlotsThisFrame = 0;
	uint32_t acquiredSlots = 0;
	uint32_t peakAcquiredSlots = 0;
	uint32_t slotRefusals = 0;

<<<<<<< HEAD
	std::vector<CpuTimer> activeCpuTimers;
	std::vector<CompletedCpuTimer> completedCpuTimers;

	/// Drains the oldest in-flight frame's results if resolved; returns false
	/// only when GPU data is still pending (retry next frame), never when
	/// there's simply nothing to collect.
	bool CollectResults();
	void PublishCpuResults(std::string_view a_namePrefix, uint32_t a_frameCount, uint64_t a_captureCycle);
	void RebuildResults();

	static bool HasCaptureMode(CaptureMode a_value, CaptureMode a_mode)
	{
		return (static_cast<uint8_t>(a_value) & static_cast<uint8_t>(a_mode)) != 0;
	}

	bool MatchesActiveFilter(std::string_view a_name) const
	{
		return activeNamePrefix.empty() || a_name.starts_with(activeNamePrefix);
	}

	void LatchCaptureRequest();
||||||| 12b261724
	void CollectResults();
=======
	/** @brief Pushes each resolved frame's samples into the rows and publishes the newest. */
	void CollectResults();
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

	/** @brief Drops timers that have not been sampled for kTimerRetireFrames, so disabled passes stop reporting stale values. */
	void RetireStaleTimers();

	/** @brief Repoints knownTimerIndex at the current knownTimers positions after an erase. */
	void RebuildTimerIndex();

	KnownTimer& GetOrCreateTimer(const std::string& name);

	/// Clears a ring slot's transient GPU state so a later ClearTimers
	/// call can't have it resurrect a stale sample once it resolves.
	static void ResetFrameState(FrameQueries& frame)
	{
		frame.batch.Reset();
		frame.activeStack.clear();
		frame.captureCycle = false;
		frame.acquiredTimerCount = 0;
		frame.inFlight = false;
		frame.capturedCycle = 0;
		frame.namePrefix.clear();
	}

	/// Resets every ring slot except one with an open scope: frames[writeFrame]
	/// while frameActive, whose batch/activeStack a still-open BeginPass uses.
	void ResetPendingFrames()
	{
		for (uint32_t i = 0; i < kFrameLatency; i++) {
			if (frameActive && i == writeFrame)
				continue;
			ResetFrameState(frames[i]);
		}
	}
};

/**
 * @brief RAII CPU-only profiling scope; pairs with CS_PROFILE_CPU_SCOPE.
 */
class ScopedCpuPass
{
public:
	ScopedCpuPass(Profiler* a_profiler, std::string_view a_name)
	{
		// End only scopes that opened successfully.
		if (a_profiler && a_profiler->BeginCpuPass(a_name))
			profiler = a_profiler;
	}
	~ScopedCpuPass()
	{
		if (profiler)
			profiler->EndCpuPass();
	}

	ScopedCpuPass(const ScopedCpuPass&) = delete;
	ScopedCpuPass& operator=(const ScopedCpuPass&) = delete;

private:
	Profiler* profiler = nullptr;
};

/// Times a CPU-only scope (no GPU query); see ScopedCpuPass.
#define CS_PROFILE_CPU_SCOPE(profiler, name) \
	ScopedCpuPass CS_DETAIL_CONCAT(cs_profile_cpu_scope_, __LINE__) { profiler, name }

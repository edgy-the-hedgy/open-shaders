#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "Profiler.h"
#include "Utils/Game.h"
#include "Utils/LegitProfiler.h"

/**
 * @brief Renders GPU and CPU profiling statistics, timing graphs, and per-feature timer breakdowns.
 *
 * Provides the Profiling page in the menu with frame time graphs, sortable
 * timing tables grouped by rendering pass, the cost chip in each feature's
 * header with its per-feature popover, and the Performance Overlay's filtered
 * render-pass graph.
 */
class ProfilingRenderer
{
public:
	/** @brief Selects whether profiling displays GPU or CPU timing data. */
	enum class TimingMode
	{
		GPU,
		CPU
	};

	/** @brief Per-feature profiling state: disabled, or displaying GPU/CPU timings. */
	enum class FeatureTimingMode
	{
		Off,
		GPU,
		CPU
	};

	/**
	 * @brief Renders the main profiling statistics view with timing graph and pass table.
	 *
	 * Draws a frame time graph, a GPU/CPU mode toggle, and a grouped table of
	 * per-pass timing statistics with average, P95, and P99 columns.
	 *
	 * @param showTable If true, renders the detailed timing table below the graph.
	 * @param showModeToggle If true, renders the GPU/CPU timing mode toggle.
	 */
	static void RenderStatistics(bool showTable = true, bool showModeToggle = true);

	/**
<<<<<<< HEAD
	 * @brief Renders per-feature profiling through a bottom-aligned disclosure.
||||||| 12b261724
	 * @brief Renders timer entries for a specific feature as an inline graph and table.
=======
	 * @brief Draws a feature's cost chip at the cursor: its total average time, which opens a popover with the
	 * graph, the per-pass table and the overlay toggle. Draws nothing when the feature has no timers or the chip
	 * would cross `rightLimit`.
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	 *
<<<<<<< HEAD
	 * A compact Profiling button uses the bottom of the window on short pages
	 * and follows the end of the feature material on long pages. Active timing
	 * data appears above the CPU, GPU, and Off controls, with the button beneath
	 * both. A small gap separates profiling from feature settings.
||||||| 12b261724
	 * Filters profiler results to those matching the given feature prefix and
	 * displays them with a dedicated per-feature timing graph.
=======
	 * Call it every frame the feature page is shown: it also records the feature's graph, so the popover opens
	 * on a filled history.
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	 *
<<<<<<< HEAD
	 * @param featurePrefix The profiler timer name prefix identifying the feature
	 *        (e.g. "ScreenSpaceGI").
	 * @pre PrepareFeatureTimers was called for the same feature this frame.
||||||| 12b261724
	 * @param featurePrefix The profiler timer name prefix identifying the feature
	 *        (e.g. "ScreenSpaceGI").
=======
	 * @param featurePrefix The profiler timer name prefix identifying the feature (e.g. "ScreenSpaceGI").
	 * @param rightLimit Screen x the chip must end before.
	 * @return True when the chip was drawn.
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	 */
	static bool RenderFeatureCostChip(const std::string& featurePrefix, float rightLimit);

	/**
	 * @brief Prepares per-feature timing data and returns its rendered height.
	 *
	 * @param featurePrefix The profiler timer name prefix identifying the feature.
	 * @param showProfiling Whether profiling controls are available on the active page.
	 * @return Height of the prepared profiling content.
	 */
	static float PrepareFeatureTimers(const std::string& featurePrefix, bool showProfiling);

	/** @brief Clears prepared timing data when leaving a feature page. */
	static void DeactivateFeatureTimers();

	/**
	 * @brief Checks whether per-feature profiling can be shown.
	 *
	 * True when an executed GPU pass declared this feature prefix or a legacy
	 * profiler timer has produced a matching result.
	 *
	 * @param featurePrefix The profiler timer name prefix identifying the feature.
	 */
	static bool IsFeatureProfilingAvailable(const std::string& featurePrefix);

	/**
	 * @brief Renders the Performance Overlay's render-pass graph, leaving out the groups it hides.
	 *
	 * Uses its own graph history, so the Profiling page's unfiltered graph is unaffected.
	 */
	static void RenderOverlayGraph();

	/**
	 * @brief Gets the profiler groups (timer name prefixes before "::") reporting right now, in pass order.
	 */
	static std::vector<std::string> GetActiveGroups();

	/**
	 * @brief Gets the name to show for a profiler group: its feature's display name, or the group itself.
	 * @param group Profiler group, which for a feature is its short name.
	 */
	static std::string GetGroupDisplayName(const std::string& group);

	/**
	 * @brief Draws a checkbox that shows or hides a profiler group in the Performance Overlay's graph.
	 * Draws nothing while the Performance Overlay is not loaded.
	 * @param group Profiler group, which for a feature is its short name.
	 */
	static void RenderOverlayGroupToggle(const std::string& group);

	/**
	 * @brief Opens the Profiling page with a group expanded and scrolled into view.
	 * @param group Profiler group, which for a feature is its short name.
	 */
	static void OpenProfilingPage(const std::string& group);

private:
	static inline TimingMode timingMode = TimingMode::GPU;
	static inline float timeSinceLastUpdate = 0.0f;
	static inline float lastFrameTime = 0.0f;

	struct PassEntry
	{
		std::string label;
		float avgMs;
		float p95Ms;
		float p99Ms;
	};
	struct GroupEntry
	{
		std::string name;
		float totalAvgMs = 0.0f;
		float totalP95Ms = 0.0f;
		float totalP99Ms = 0.0f;
		std::vector<PassEntry> passes;
	};
	static inline float cachedTotalAvgMs = 0.0f;
	static inline float cachedMaxAvgMs = 0.0f;
	static inline float cachedMaxP95Ms = 0.0f;
	static inline float cachedMaxP99Ms = 0.0f;
	static inline std::vector<GroupEntry> cachedGroups;

	static inline ImGuiUtils::ProfilerGraph gpuGraph{ Profiler::kHistorySize };
	static inline Util::GraphSampleGate gpuGraphGate;

	struct FeatureGraphState
	{
		ImGuiUtils::ProfilerGraph gpuGraph{ Profiler::kHistorySize };
		ImGuiUtils::ProfilerGraph cpuGraph{ Profiler::kHistorySize };
		Util::GraphSampleGate gpuGate;
		Util::GraphSampleGate cpuGate;
	};
	static inline std::unordered_map<std::string, FeatureGraphState> featureGraphs;
	static inline std::unordered_map<std::string, FeatureTimingMode> featureTimingModes;

	struct FeatureTimingEntry
	{
		std::string label;
		/// Feature-qualified name, so two features' same-named passes (e.g.
		/// both having a "Downsample") don't collide onto the same color.
		std::string colorKey;
		float timeMs;
		float avgMs;
		float p95Ms;
		float p99Ms;
	};
	struct FeatureTimingData
	{
<<<<<<< HEAD
		std::vector<FeatureTimingEntry> entries;
		float totalAvg = 0.0f;
		float totalP95 = 0.0f;
		float totalP99 = 0.0f;
		float maxAvg = 0.0f;
		float maxP95 = 0.0f;
		float maxP99 = 0.0f;
||||||| 12b261724
		ImGuiUtils::ProfilerGraph graph;
		Util::GraphSampleGate gate;
		double lastStatisticsUpdate = -1.0;
		std::unordered_map<std::string, PassStatistics> statistics;

		// User-provided so the static members below can default-construct it in-class.
		GraphState() :
			graph(Profiler::kHistorySize) {}

		/** @brief Records this frame's tasks (if the gate allows) and renders the graph. */
		void Render(const std::vector<legit::ProfilerTask>& tasks, float baseLegendWidth, float baseHeight, float minFrameTimeSec);
=======
		ImGuiUtils::ProfilerGraph graph;
		Util::GraphSampleGate gate;
		double lastStatisticsUpdate = -1.0;
		std::unordered_map<std::string, PassStatistics> statistics;

		// User-provided so the static members below can default-construct it in-class.
		GraphState() :
			graph(Profiler::kHistorySize) {}

		/** @brief Records this frame's tasks, if the gate allows. */
		void Record(const std::vector<legit::ProfilerTask>& tasks);

		/** @brief Draws the recorded history. */
		void Draw(float baseLegendWidth, float baseHeight, float minFrameTimeSec);

		/** @brief Records this frame's tasks (if the gate allows) and draws the graph. */
		void Render(const std::vector<legit::ProfilerTask>& tasks, float baseLegendWidth, float baseHeight, float minFrameTimeSec);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	};
<<<<<<< HEAD
	struct FeatureTimingViewState
	{
		bool controlsVisible = false;
		bool profilingDisabled = false;
		FeatureTimingMode mode = FeatureTimingMode::Off;
		FeatureTimingData data;
		float contentHeight = 0.0f;
	};
	static inline std::unordered_map<std::string, FeatureTimingViewState> featureTimingViews;
||||||| 12b261724
	static inline GraphState gpuGraph;
	static inline std::unordered_map<std::string, GraphState> featureGraphs;
=======
	static inline GraphState gpuGraph;
	static inline GraphState overlayGraph;
	static inline std::unordered_map<std::string, GraphState> featureGraphs;
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

<<<<<<< HEAD
	static ImU32 GetGroupColor(std::string_view groupName);
||||||| 12b261724
	static inline std::unordered_map<std::string, ImU32> groupColorMap;
	static inline size_t nextColorIndex = 0;

	static ImU32 GetGroupColor(const std::string& groupName);
=======
	/** @brief One of a feature's passes, timed in the current mode. */
	struct FeatureTimerEntry
	{
		std::string label;
		float timeMs;
		float avgMs;
		float p95Ms;
		float p99Ms;
	};

	/** @brief A feature's passes this frame with their totals and column maxima. */
	struct FeatureTimings
	{
		std::vector<FeatureTimerEntry> entries;
		float totalAvgMs = 0.0f;
		float totalP95Ms = 0.0f;
		float totalP99Ms = 0.0f;
		float maxAvgMs = 0.0f;
		float maxP95Ms = 0.0f;
		float maxP99Ms = 0.0f;
	};

	/** @brief Group the Profiling page expands and scrolls to on its next draw; empty when none. */
	static inline std::string focusGroup;

	static inline std::unordered_map<std::string, ImU32> groupColorMap;
	static inline size_t nextColorIndex = 0;

	static ImU32 GetGroupColor(const std::string& groupName);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	static uint32_t ToLegitColor(ImU32 imColor);
	static ImVec4 HeatColor(float value, float maxValue);
	static void TextHeat(const char* fmt, float value, float maxValue);
	static void RenderTimingModeToggle();
	static void SetupTimingTableColumns(float passColumnWidth, bool includePercentColumn);
	static void RenderGraph();
	static void UpdateStatistics(bool cpuMode);
	static std::string GetFeatureTimerPrefix(const std::string& featurePrefix);
	static std::string GetGroupName(std::string_view timerName);
	/** @brief Gathers a feature's timings in the current mode, refreshing its cached statistics once a second. */
	static FeatureTimings CollectFeatureTimings(const std::string& featurePrefix, GraphState& state);
	static std::vector<legit::ProfilerTask> BuildFeatureTasks(const std::string& featurePrefix, const FeatureTimings& timings);
	/** @brief The popover's body: mode toggle, graph, per-pass table, overlay toggle and the Profiling page link. */
	static void RenderFeatureTimersPopover(const std::string& featurePrefix, GraphState& state, const FeatureTimings& timings);
	static bool IsFeatureTimerResult(const Profiler::TimerResult& result, std::string_view prefix);

	static FeatureTimingData CollectFeatureTimingData(const std::string& featurePrefix, bool cpuMode);
	static float GetFeatureTimingDataHeight(const FeatureTimingData& data);
	static int ComputeFeatureGraphLegendWidth(const FeatureTimingData& data, int totalWidth);
	static bool RenderFeatureTimingGraph(const FeatureTimingData& data, ImGuiUtils::ProfilerGraph& graph, Util::GraphSampleGate& gate, float graphHeight);
	static void RenderFeatureTimingData(const std::string& featurePrefix, FeatureTimingMode featureMode, const FeatureTimingData& data);
	static void RenderFeatureTimingModeControls(const std::string& featurePrefix, FeatureTimingMode& featureMode);
	static void RenderFeatureTimingButton(const std::string& featurePrefix);
	static bool RenderFeatureOverview();
};

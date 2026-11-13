#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

<<<<<<< HEAD
// Status panel for the devbench bridge. The plugin's tools are registered into the external
// devbench host via DevBenchBridge (see src/Features/RemoteControl/DevBenchBridge.cpp); this feature surfaces, in the
// in-game menu, whether devbench is present, what was registered, and which port the host bound.
class RemoteControl : public Feature
||||||| 12b261724
// Status panel for the devbench bridge. The plugin's tools are registered into the external
// devbench host via DevBenchBridge (see src/Features/RemoteControl/DevBenchBridge.cpp); this feature surfaces, in the
/**
 * @brief Feature class for integrating Community Shaders with an external devbench host.
 * 
 * Exposes shader tools (feature, inspect, shadercache, capture, settings) to AI assistants
 * through a shared devbench bench, accessible via MCP and REST protocols. Uses a singleton
 * pattern to ensure a single instance. Initialization is deferred to DataLoaded to allow
 * devbench's cross-plugin interface to be ready before the bridge is installed.
 */

/**
 * Retrieves the singleton instance of RemoteControl.
 * @return Pointer to the RemoteControl singleton instance.
 */

/**
 * Initializes the devbench bridge.
 * 
 * Called at DataLoaded rather than Load to ensure devbench's kPostLoad initialization
 * has completed and its cross-plugin interface is available.
 */

/**
 * Renders the feature's status and configuration panel in the in-game menu.
 */
class RemoteControl : public Feature
=======
namespace NvPerf
{
	struct CaptureResult;
}

/**
 * @brief devbench bridge and Nsight Perf panels. A component of the Developer Tools feature, not a feature itself.
 *
 * Exposes shader tools (feature, inspect, shadercache, capture, nvperf, settings) to AI assistants
 * through the external devbench host (see src/Features/RemoteControl/DevBenchBridge.cpp), accessible
 * via MCP and REST. Also hosts the Nsight Perf capture controls (Features/RemoteControl/NvPerf).
 */
class RemoteControl
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
{
public:
	static RemoteControl* GetSingleton();

<<<<<<< HEAD
	// Feature overrides — see Feature.h for contracts. Only members that differ from the base
	// defaults are overridden (the feature has no settings, shaders, or per-frame state).
	std::string GetName() override { return "Remote Control"; }
	std::string GetShortName() override { return "RemoteControl"; }
	std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	bool IsInMenu() const override { return false; }
	bool SupportsVR() override { return true; }

	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Expose Open Shaders to AI assistants through the external devbench host.",
			{
				"Registers feature, inspect, shadercache, capture, nvperf, and settings tools",
				"Drivable over MCP and REST from the shared devbench bench",
				"No in-game server — install the devbench plugin to enable",
				"Nsight Perf GPU counters per render pass (NVIDIA, builds with NVPERF_SUPPORT)",
			}
		};
	}
||||||| 12b261724
	// Feature overrides — see Feature.h for contracts. Only members that differ from the base
	// defaults are overridden (the feature has no settings, shaders, or per-frame state).
	std::string GetName() override { return "Remote Control"; }
	std::string GetShortName() override { return "RemoteControl"; }
	std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Expose Community Shaders to AI assistants through the external devbench host.",
			{
				"Registers feature, inspect, shadercache, capture, nvperf, and settings tools",
				"Drivable over MCP and REST from the shared devbench bench",
				"No in-game server — install the devbench plugin to enable",
				"Nsight Perf GPU counters per render pass (NVIDIA, builds with NVPERF_SUPPORT)",
			}
		};
	}
=======
	/**
	 * @brief Registers the plugin's tools into the devbench host. Called from DeveloperTools::DataLoaded.
	 *
	 * Runs at DataLoaded rather than Load: Load runs during SKSEPluginLoad, before devbench's
	 * kPostLoad init, so its cross-plugin interface isn't ready yet.
	 */
	void DataLoaded();
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

<<<<<<< HEAD
	void PostPostLoad() override;
	void DrawSettings() override;
||||||| 12b261724
	// The bridge installs at DataLoaded (not Load): Load runs during SKSEPluginLoad, before
	// devbench's kPostLoad init, so its cross-plugin interface isn't ready yet.
	void DataLoaded() override;
	void DrawSettings() override;
=======
	/** @brief True when the devbench host's cross-plugin interface is present. */
	static bool IsDevBenchHostPresent();

	/** @brief True when this build was compiled with the devbench bridge. */
	static bool IsDevBenchBridgeBuilt();

	/** @brief Renders the devbench tab: host status, bound port and the registered tools. */
	void DrawDevBenchPanel();

	/** @brief Renders the Nsight Perf tab: GPU support, the capture's metrics and ranges, recent captures and the viewed result. */
	void DrawNvPerfPanel();

	/** @brief Port the devbench host bound, read from its runtime.json at most every two seconds; 0 when unknown. */
	static int GetDevBenchPort();

	/** @brief Starts an Nsight Perf capture with the panel's metrics, ranges and counts. @return False with the panel's error set when it could not start. */
	bool RequestNvPerfCapture();

	/** @brief Records the latest Nsight Perf result in the recent captures list when it is new. Cheap; call every frame the panel's card shows. */
	void UpdateNvPerfHistory();
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

	RemoteControl() = default;
	~RemoteControl() = default;

	RemoteControl(const RemoteControl&) = delete;
	RemoteControl& operator=(const RemoteControl&) = delete;
	RemoteControl(RemoteControl&&) = delete;
	RemoteControl& operator=(RemoteControl&&) = delete;

private:
	// The Nsight Perf section's inputs. UI state only: nothing here is saved.
	struct NvPerfPanel
	{
		std::vector<char> defaultMetricEnabled;  // per NvPerf::DefaultMetrics() entry
		std::string extraMetrics;                // comma-separated name[:kind]
		std::string ranges;                      // comma-separated range filters
		int samples = 1;
		int delayFrames = 0;
		int nestingLevels = 1;
		std::string requestError;

		/** @brief A finished capture and when it was first seen. */
		struct RecentCapture
		{
			std::shared_ptr<const NvPerf::CaptureResult> result;
			std::filesystem::file_time_type time;
		};
		std::vector<RecentCapture> recent;  // newest first
		std::uint64_t viewedCaptureId = 0;  // 0: the newest
	} nvperfPanel;

	static constexpr std::size_t kMaxRecentNvPerfCaptures = 8;

	/** @brief The recent captures table of the Nsight Perf panel. */
	void DrawRecentNvPerfCaptures(NvPerfPanel& a_panel);
};

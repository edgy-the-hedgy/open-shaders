#pragma once

#include "Feature.h"
#include "I18n/I18n.h"

struct CSEditor : Feature
{
public:
	static CSEditor* GetSingleton()
	{
		static CSEditor singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "CS Editor"; }
	virtual std::string GetDisplayName() override { return T("feature.cs_editor.name", "OS Editor"); }
	virtual inline std::string GetShortName() override { return "CSEditor"; }
	virtual inline std::string_view GetShaderDefineName() override { return "CS_EDITOR"; }
	virtual inline std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	virtual bool SupportsVR() override { return true; }
	virtual bool IsCore() const override { return true; }
	virtual bool IsInMenu() const override { return false; }

	virtual inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.cs_editor.description", "Development tool for inspecting, editing, and previewing renderer-facing data in-game."),
			{ T("feature.cs_editor.key_feature_1", "Provides weather editing functionality"),
				T("feature.cs_editor.key_feature_2", "Includes dynamic saving and loading of vanilla post processing and weather settings."),
				T("feature.cs_editor.key_feature_3", "Real-time editing and previewing of effects") } };
	}

	virtual void DataLoaded() override;
	virtual void Prepass() override;

	void DrawLauncherButton();

	static void OpenEditorWindow();
	static void ToggleEditorWindow();
	static void UpdateWeatherLockAndTime();

	void LerpWeather(RE::TESWeather*, RE::TESWeather*, float);

<<<<<<< HEAD
||||||| 12b261724
	/**
	 * Renders the standalone weather details window.
	 * @param open Pointer to the open/close state owned by the caller.
	 */
	void RenderWeatherDetailsWindow(bool* open, bool showSectionHeaders = true);

	// Core weather display functions that other features can use
	/**
	 * Displays weather info for a given weather record.
	 * @param weather Weather record to display.
	 * @param weatherPct Optional blend percentage (0-1), or -1 to hide.
	 * @param showInteractiveElements Enables interactive controls when true.
	 */
	static void DisplayWeatherInfo(RE::TESWeather* weather, float weatherPct = -1.0f, bool showInteractiveElements = true);
	/**
	 * Renders the core weather details UI section.
	 * @param showInteractiveElements Enables interactive controls when true.
	 */
	static void RenderCoreWeatherDetails(bool showInteractiveElements = true, bool showSectionHeaders = true);
	/**
	 * Renders weather analysis sections contributed by other features.
	 */
	static void RenderFeatureWeatherAnalysis();

	// --- Refactor helpers for RenderCoreWeatherDetails ---
	/**
	 * Renders the weather controls section.
	 * @param sky Active sky instance.
	 */
	static void RenderWeatherControls(RE::Sky* sky, bool showSectionHeader = true);
	/**
	 * Renders the weather information display section.
	 * @param sky Active sky instance.
	 * @param showInteractiveElements Enables interactive controls when true.
	 */
	static void RenderWeatherInformationDisplay(RE::Sky* sky, bool showInteractiveElements = true, bool showSectionHeader = true);

	struct WeatherDetailsWindowSettings
	{
		bool Enabled = false;
		bool ShowInOverlay = false;
		ImVec2 Position = ImVec2(50.f, 50.f);
		bool PositionSet = false;
	} WeatherDetailsWindow;

	/**
	 * Gets the appropriate color for a weather type based on its flags.
	 * Uses a priority system: Rain > Snow > Aurora > Aurora Follows Sun > Cloudy > Pleasant > Unclassified > Default
	 * @param weather Pointer to the weather object
	 * @return ImVec4 color appropriate for the weather type
	 */
	static ImVec4 GetWeatherTypeColor(RE::TESWeather* weather);
	/**
	 * Renders a weather name with multiple colors if the weather has multiple flags.
	 * Each flag gets its own color segment in the weather name display.
	 * @param weather Pointer to the weather object
	 * @param weatherName The formatted weather name to display
	 * @return true if the main weather name (base name) was hovered, false otherwise
	 */
	static bool RenderMultiColorWeatherName(RE::TESWeather* weather, const std::string& weatherName);

	/**
	 * Get the color associated with a specific weather flag.
	 * @param flag The weather flag to get the color for
	 * @return ImVec4 color for the flag
	 */
	static ImVec4 GetWeatherFlagColor(RE::TESWeather::WeatherDataFlag flag);

	/**
	 * Get the color associated with a specific weather flag by name.
	 * @param flagName The name of the flag to get the color for
	 * @return ImVec4 color for the flag
	 */
	static ImVec4 GetWeatherFlagColorByName(const std::string& flagName);

=======
	/**
	 * Renders the standalone weather details window.
	 * @param open Pointer to the open/close state owned by the caller.
	 */
	void RenderWeatherDetailsWindow(bool* open, bool showSectionHeaders = true);

	/**
	 * Renders the CS Editor's optional Weather Debug window: custom-settings status, filter
	 * select/clear, current/last weather details and rain & wetness analysis from other features.
	 * @param open Pointer to the open/close state owned by the caller.
	 */
	static void DrawWeatherDebugWindow(bool* open);

	// Core weather display functions that other features can use
	/**
	 * Displays weather info for a given weather record.
	 * @param weather Weather record to display.
	 * @param weatherPct Optional blend percentage (0-1), or -1 to hide.
	 * @param showInteractiveElements Enables interactive controls when true.
	 */
	static void DisplayWeatherInfo(RE::TESWeather* weather, float weatherPct = -1.0f, bool showInteractiveElements = true);
	/**
	 * Renders the core weather details UI section.
	 * @param showInteractiveElements Enables interactive controls when true.
	 */
	static void RenderCoreWeatherDetails(bool showInteractiveElements = true, bool showSectionHeaders = true);
	/**
	 * Renders weather analysis sections contributed by other features.
	 */
	static void RenderFeatureWeatherAnalysis();

	// --- Refactor helpers for RenderCoreWeatherDetails ---
	/**
	 * Renders the weather information display section.
	 * @param sky Active sky instance.
	 * @param showInteractiveElements Enables interactive controls when true.
	 */
	static void RenderWeatherInformationDisplay(RE::Sky* sky, bool showInteractiveElements = true, bool showSectionHeader = true);

	struct WeatherDetailsWindowSettings
	{
		bool Enabled = false;
	} WeatherDetailsWindow;

	/**
	 * Gets the appropriate color for a weather type based on its flags.
	 * Uses a priority system: Rain > Snow > Aurora > Aurora Follows Sun > Cloudy > Pleasant > Unclassified > Default
	 * @param weather Pointer to the weather object
	 * @return ImVec4 color appropriate for the weather type
	 */
	static ImVec4 GetWeatherTypeColor(RE::TESWeather* weather);
	/**
	 * Renders a weather name with multiple colors if the weather has multiple flags.
	 * Each flag gets its own color segment in the weather name display.
	 * @param weather Pointer to the weather object
	 * @param weatherName The formatted weather name to display
	 * @return true if the main weather name (base name) was hovered, false otherwise
	 */
	static bool RenderMultiColorWeatherName(RE::TESWeather* weather, const std::string& weatherName);

	/**
	 * Get the color associated with a specific weather flag.
	 * @param flag The weather flag to get the color for
	 * @return ImVec4 color for the flag
	 */
	static ImVec4 GetWeatherFlagColor(RE::TESWeather::WeatherDataFlag flag);

	/**
	 * Get the color associated with a specific weather flag by name.
	 * @param flagName The name of the flag to get the color for
	 * @return ImVec4 color for the flag
	 */
	static ImVec4 GetWeatherFlagColorByName(const std::string& flagName);

	// Accessors for Weather Picker Window
	static std::vector<RE::TESWeather*>& GetFilteredWeathers() { return s_filteredWeathers; }
	static int& GetSelectedWeatherIdx() { return s_selectedWeatherIdx; }
	static int FindWeatherIndex(RE::TESWeather* targetWeather);

	// Shared weather-control backend, used by both the feature page and the Weather Picker Window.
	/** @brief Rebuilds the filtered weather list (and clears the selection) if the flag filter changed. */
	static void SyncWeatherFilter();
	/** @brief Switches to the weather at @p filteredIdx of the filtered list, honouring accelerate and the weather lock. */
	static void SelectWeather(RE::Sky* sky, int filteredIdx);
	/** @brief Returns the sky to its default weather, releasing the weather lock first so the reset can take hold. */
	static void ResetWeatherToDefault(RE::Sky* sky);
	/**
	 * Draws the transition progress bar for the current -> last weather blend.
	 * @param width Bar width in pixels (negative fills the available width).
	 * @param verboseOverlay Include the "Transitioning from X" text in the overlay.
	 */
	static void DrawWeatherTransitionBar(RE::Sky* sky, float width, bool verboseOverlay);
	/**
	 * Draws the weather-type filter chips plus the accelerate toggle as a 2-column grid of
	 * colour-coded text toggles (dim = off, bright = on). Updates the shared filter state.
	 */
	static void DrawWeatherFilterChips();

	// Implement OverlayFeature interface
	void DrawOverlay() override;
	bool IsOverlayVisible() const override;

	/** @brief Re-resolves form edits after the active preset pack changed, reloading the built widgets it affects. */
	static void ReloadFormEdits();
	/** @brief Whether a form edit file exists, the user's or the active pack's; cached until ReloadFormEdits. */
	static bool HasWidgetJsonFiles();

>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
private:
<<<<<<< HEAD
||||||| 12b261724
	void DrawShowInOverlayToggle();
	void DrawTimeControls();
	void DrawWeatherStatusPanel();
	void DrawWeatherPickerSection();

	// Wind direction offset to align with game's coordinate system
	static constexpr float WIND_DIRECTION_OFFSET = 30.5f;

	// Weather flag filter bits (for 7 weather types)
	static constexpr uint32_t ALL_WEATHER_FLAGS = 0x7F;  // Bits 0-6 all enabled
	static constexpr uint32_t UNCLASSIFIED_FLAG = 0x40;  // Bit 6 only

	// Static state for weather picker and data
=======
	void DrawTimeControls();

	// Wind direction offset to align with game's coordinate system
	static constexpr float WIND_DIRECTION_OFFSET = 30.5f;

	// Weather flag filter bits (for 7 weather types)
	static constexpr uint32_t ALL_WEATHER_FLAGS = 0x7F;  // Bits 0-6 all enabled
	static constexpr uint32_t UNCLASSIFIED_FLAG = 0x40;  // Bit 6 only

	// Static state for weather picker and data
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	static inline bool s_dataAvailable = false;
	static inline bool s_resourcesInitialized = false;
	static inline bool s_checkedWidgetJsonFiles = false;
	static inline bool s_hasWidgetJsonFiles = false;

<<<<<<< HEAD
	static bool HasWidgetJsonFiles();
||||||| 12b261724
	// Static helper for display name extraction
	static std::string GetDisplayName(const RE::TESWeather* weather);

	// Weather comparator for consistent sorting
	struct WeatherNameComparator
	{
		bool operator()(const RE::TESWeather* a, const RE::TESWeather* b) const
		{
			return CSEditor::GetDisplayName(a) < CSEditor::GetDisplayName(b);
		}
	};

	// --- Refactor helpers for DisplayWeatherInfo ---
	static void DisplayWeatherBasicInfo(RE::TESWeather* weather, float weatherPct);
	static void DisplayPrecipitationInfo(RE::TESWeather* weather);
	static void DisplayLightningInfo(RE::TESWeather* weather, bool showInteractiveElements);
	static void DisplayWindInfo(RE::TESWeather* weather);

	// Helper functions
	static bool HasWidgetJsonFiles();
=======
	// Static helper for display name extraction
	static std::string GetDisplayName(const RE::TESWeather* weather);

	// Weather comparator for consistent sorting
	struct WeatherNameComparator
	{
		bool operator()(const RE::TESWeather* a, const RE::TESWeather* b) const
		{
			return CSEditor::GetDisplayName(a) < CSEditor::GetDisplayName(b);
		}
	};

	// --- Refactor helpers for DisplayWeatherInfo ---
	static void DisplayWeatherBasicInfo(RE::TESWeather* weather, float weatherPct);
	static void DisplayPrecipitationInfo(RE::TESWeather* weather);
	static void DisplayLightningInfo(RE::TESWeather* weather, bool showInteractiveElements);
	static void DisplayWindInfo(RE::TESWeather* weather);

	// Helper functions
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	static bool ShouldPreloadEditorResources();
	static void EnsureDataLoaded();
<<<<<<< HEAD
||||||| 12b261724
	static void LoadAllWeathers();
	static void UpdateFilteredWeathers();
	static int FindWeatherIndex(RE::TESWeather* targetWeather);
	static std::vector<std::string> GetWeatherFlagNames(RE::TESWeather* weather);

	// Implement OverlayFeature interface
	void DrawOverlay() override;
	bool IsOverlayVisible() const override;
=======
	static void LoadAllWeathers();
	static void UpdateFilteredWeathers();
	static std::vector<std::string> GetWeatherFlagNames(RE::TESWeather* weather);
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
};

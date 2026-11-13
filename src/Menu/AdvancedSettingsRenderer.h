#pragma once

#include "Menu/SettingsSections.h"

#include <span>

/**
 * @brief Draws the Advanced sections of the Settings page (see SettingsPageRenderer): developer tools,
 * logging, shader debugging and A/B testing, plus the compiler controls of Shaders & Cache.
 */
class AdvancedSettingsRenderer
{
public:
	/** @brief Searchable setting labels of the sections drawn here. */
	static std::span<const SettingsSearchEntry> GetSearchIndex();

<<<<<<< HEAD
private:
	static void RenderShadersSection();
	static void RenderDiagnosticsSection();
	static void RenderDisableAtBootSection(const std::function<void()>& drawDisableAtBootSettings);
||||||| 12b261724
private:
	static void RenderLoggingSection();
	static void RenderShaderDebugSection();
	static void RenderDisableAtBootSection(const std::function<void()>& drawDisableAtBootSettings);
	static void RenderDeveloperSection();
=======
	/** @brief Compiler thread counts, Dump Shaders and Clear Shader Cache, drawn in the Shaders & Cache section. */
	static void RenderCompilerSection();
	/** @brief Log level, shader defines, and the log and ini dump buttons. */
	static void RenderLoggingSection();
	/** @brief Shader replacement toggles and the shader blocking switch. */
	static void RenderShaderDebugSection();
	/** @brief The shaders used in recent frames, with the blocked shader's details; a row click blocks that shader. */
	static void RenderActiveShadersSection();
	/** @brief Developer mode, file watcher, compiler flags, the content store and build statistics. */
	static void RenderDeveloperSection();
	/** @brief A/B testing controls. */
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb
	static void RenderTestingSection();
<<<<<<< HEAD

	// Helpers used by the sections above
	static void RenderShaderCompileFlags();
	static void RenderShaderThreading();
	static void RenderShaderCacheControls();
	static void RenderShaderReplacementTable();
	static void RenderShaderCompileStatistics();

	static void RenderLoggingControls();
	static void RenderRuntimeDebugControls();
	static void RenderShaderBlockingPanel();
	static void RenderCompileTraceExport();
};
||||||| 12b261724
};
=======

	/**
	 * @brief Puts the player in a known test scene: fast, invulnerable, no collision, time paused at noon, in Whiterun
	 * under a forced weather. Does nothing unless a game is loaded and a menu is open (the console needs both).
	 */
	static void RunTestConditions();

	/** @brief Tooltip for the Test Conditions button, listing the console commands it runs. */
	static const char* GetTestConditionsTooltip();
};
>>>>>>> c393c10e29a46d85eb61e74bde6b59c39f4582bb

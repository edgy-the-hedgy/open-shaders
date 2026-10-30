#pragma once

#include <algorithm>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace RayRegeneration
{
	/** @brief Owns shared SDK overrides without copying JSON on every reconstruction dispatch. */
	struct Configuration
	{
		nlohmann::json overrides = nlohmann::json::object();
		uint64_t revision = 0;
	};
	/** @brief Returns the shared SDK configuration persisted by Upscaling. */
	inline Configuration& GetConfiguration()
	{
		static Configuration configuration;
		return configuration;
	}
	/** @brief Publishes validated SDK overrides and invalidates active reconstruction histories. */
	inline void SetConfiguration(const nlohmann::json& a_overrides)
	{
		auto& configuration = GetConfiguration();
		if (configuration.overrides == a_overrides) return;
		configuration.overrides = a_overrides;
		++configuration.revision;
	}
	/** @brief Reconstruction policy shared by producers without depending on the Upscaling feature. */
	struct Settings
	{
		uint32_t resolutionMode = 1;
		bool checkerboard = true;
		bool enabled = true;
		uint64_t revision = 0;
	};
	/** @brief Returns the reconstruction policy edited and persisted by Upscaling. */
	inline Settings& GetSettings()
	{
		static Settings settings;
		return settings;
	}
	/** @brief Applies bounded reconstruction controls and invalidates all producer histories. */
	inline void Configure(uint32_t a_resolution, bool a_checkerboard, bool a_enabled)
	{
		auto& settings = GetSettings();
		a_resolution = std::min(a_resolution, 2u);
		if (settings.resolutionMode == a_resolution && settings.checkerboard == a_checkerboard && settings.enabled == a_enabled) return;
		settings.resolutionMode = a_resolution;
		settings.checkerboard = a_checkerboard;
		settings.enabled = a_enabled;
		++settings.revision;
	}
}

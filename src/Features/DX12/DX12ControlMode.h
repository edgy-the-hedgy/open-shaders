#pragma once

#include <algorithm>

namespace DX12
{
	// Persisted mode IDs; keep these stable independently of dropdown order.
	enum class ControlMode : int
	{
		ShadowOffload = 0,
		RasterComparison = 1,
		FullRT = 2,
		FullNative = 3
	};

	constexpr ControlMode SanitizeControlMode(int mode, bool developerMode)
	{
		const auto selected = mode == static_cast<int>(ControlMode::FullNative) ? ControlMode::FullNative : static_cast<ControlMode>(std::clamp(mode,
			static_cast<int>(ControlMode::ShadowOffload), static_cast<int>(ControlMode::FullRT)));
		return selected == ControlMode::RasterComparison && !developerMode ? ControlMode::ShadowOffload : selected;
	}
	constexpr bool RequiresMaterialPreparation(ControlMode mode) { return mode == ControlMode::FullNative; }

	constexpr bool RequiresModeRestart(ControlMode active, ControlMode requested)
	{
		return RequiresMaterialPreparation(active) != RequiresMaterialPreparation(requested);
	}
}

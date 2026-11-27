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
	// Authority is session-wide; availability of native GPU work is a separate concern.
	enum class FeatureBackend
	{
		Baseline,
		FullNative
	};

	constexpr FeatureBackend SelectFeatureBackend(bool active, ControlMode mode, bool isVR)
	{
		return active && mode == ControlMode::FullNative && !isVR ? FeatureBackend::FullNative : FeatureBackend::Baseline;
	}

	// Shared between shader permutation routing and disk-cache identity.
	enum class InputShaderKind { Other, Lighting, Effect, Sky, DistantTree };
	constexpr bool HasFullNativeInput(FeatureBackend backend, InputShaderKind kind)
	{
		return backend == FeatureBackend::FullNative && kind != InputShaderKind::Other;
	}

	constexpr bool RequiresMaterialPreparation(ControlMode mode) { return mode == ControlMode::FullNative; }

	constexpr bool RequiresModeRestart(ControlMode active, ControlMode requested)
	{
		return RequiresMaterialPreparation(active) != RequiresMaterialPreparation(requested);
	}
}

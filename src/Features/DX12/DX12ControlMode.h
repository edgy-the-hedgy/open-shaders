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

	// Full Native owns all four counterpart contributions exclusively.
	// Vanilla raster fallback does not activate legacy feature producers.
	enum class CounterpartKind { Shared, NativeProducer, LegacyMaterial, LegacyLighting };
	enum class ExecutionRole { Baseline, NativeAuthority, RasterFallback, Dormant };
	constexpr ExecutionRole SelectExecutionRole(FeatureBackend backend, CounterpartKind kind)
	{
		if (backend == FeatureBackend::Baseline)
			return kind == CounterpartKind::NativeProducer ? ExecutionRole::Dormant : ExecutionRole::Baseline;
		switch (kind) {
		case CounterpartKind::NativeProducer: return ExecutionRole::NativeAuthority;
		case CounterpartKind::LegacyMaterial: return ExecutionRole::Dormant;
		case CounterpartKind::LegacyLighting: return ExecutionRole::Dormant;
		default: return ExecutionRole::Baseline;
		}
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

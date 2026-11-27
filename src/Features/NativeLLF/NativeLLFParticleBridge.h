#pragma once

#include "Features/LightLimitFix.h"

#include <span>

namespace NativeLLF
{
	// Transitional bridge from the existing LLF particle expansion point. No GPU
	// raster buffers are retained. Replace with native capture at shared hooks.
	void ResetLegacyParticleBridge();
	void PublishLegacyParticleBridge(std::span<const LightLimitFix::LightData> lights,
		const RE::NiPoint3& eyePosition, uint32_t frame, uint32_t pluginFrame);
}

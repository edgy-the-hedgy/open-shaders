#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace NativeLLF::Policy
{
	inline constexpr uint32_t kMetadataRetentionFrames = 120;
	inline constexpr uint32_t kMetadataCapacityMultiplier = 4;

	struct AuthoredLight
	{
		uint32_t formID = 0;
		bool inverseSquare = false, spot = false;
		float cutoff = 1.0f, size = 1.0f;
	};

	inline AuthoredLight MakeAuthoredLight(uint32_t formID, bool inverseSquare, bool spot, float cutoff, float fov)
	{
		return { formID, inverseSquare, spot,
			std::isfinite(cutoff) ? std::clamp(cutoff, 0.01f, 1.0f) : 1.0f,
			std::isfinite(fov) ? std::clamp(fov >= 50.0f ? std::numbers::sqrt2_v<float> : fov, 0.01f, 50.0f) : 1.0f };
	}

	constexpr bool Expired(uint32_t current, uint32_t lastSeen)
	{
		return current - lastSeen > kMetadataRetentionFrames;
	}
}

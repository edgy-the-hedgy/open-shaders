#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace DX12Native::LightMath
{
	inline float ToLinear(float value, bool alreadyLinear)
	{
		value = std::max(value, 0.0f);
		return alreadyLinear ? value : (value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f));
	}

	// Match prepared and live point emitters without treating unrelated known
	// placed references as aliases.
	inline bool SameEmitterCandidate(uint32_t preparedReference, uint32_t liveReference,
		float distanceSquared, float radius)
	{
		if (preparedReference && liveReference && preparedReference != liveReference)
			return false;
		const float tolerance = std::clamp(radius * 0.01f, 4.0f, 32.0f);
		return std::isfinite(distanceSquared) && distanceSquared >= 0.0f &&
			std::isfinite(radius) && radius > 0.0f && distanceSquared <= tolerance * tolerance;
	}

	inline float MatchTolerance(float radius)
	{
		return std::clamp(radius * 0.01f, 4.0f, 32.0f);
	}
}

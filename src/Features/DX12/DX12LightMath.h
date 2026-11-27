#pragma once
#include <algorithm>
#include <cmath>

namespace DX12Native::LightMath
{
	inline float ToLinear(float value, bool alreadyLinear)
	{
		value = std::max(value, 0.0f);
		return alreadyLinear ? value : (value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f));
	}

	inline float MatchTolerance(float radius)
	{
		return std::clamp(radius * 0.01f, 4.0f, 32.0f);
	}
}

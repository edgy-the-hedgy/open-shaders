#pragma once

#include <cstdint>
#include <d3d11.h>

namespace DX12Native
{
	enum class EnvironmentEncoding : uint32_t { LegacyScene, LinearRec709, LinearAP1 };

	/** @brief Capture identity independent of the per-frame material generation. */
	struct EnvironmentStamp
	{
		uint64_t epoch = 0;
		uint32_t scope = 0, engineFrame = 0, pluginFrame = 0;
		EnvironmentEncoding encoding = EnvironmentEncoding::LegacyScene;
		bool interior = false;
		bool skyHidden = false;

		bool SameEnvironment(const EnvironmentStamp& other) const
		{
			return epoch && epoch == other.epoch && scope && scope == other.scope &&
				interior == other.interior && skyHidden == other.skyHidden && encoding == other.encoding;
		}
	};

	/** @brief Callback-scoped borrow of a completed DX11 producer revision. */
	struct EnvironmentSource
	{
		ID3D11Texture2D* texture = nullptr;
		uint64_t revision = 0;
		EnvironmentStamp stamp{};
		bool IsValidFor(const EnvironmentStamp& requested) const
		{
			return texture && revision && stamp.SameEnvironment(requested);
		}
	};
}

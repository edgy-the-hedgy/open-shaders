#pragma once

#include <d3d11.h>
#include <array>
#include <cstdint>

namespace LightLimitFixData
{
	inline constexpr uint32_t kMaximumLights = 1024;
	inline constexpr uint32_t kRecordBytes = 96;

	/** @brief Borrows the evaluated light table for immediate render-thread GPU copying in the stamped frame. */
	struct LightView
	{
		ID3D11ShaderResourceView* records = nullptr;
		std::array<float, 3> eyeOrigin{};
		uint32_t count = 0;
		uint32_t frame = 0;
		uint32_t roomGeneration = 0;
		std::array<uint32_t, 128> roomReferences{};
	};
}

#pragma once
#include <array>
#include <cstdint>
#include <imgui.h>

namespace RayRegeneration
{
	/** @brief Edits independent RR requests without changing producer or resolution settings. */
	inline void DrawSignalMask(const char* id, uint32_t& mask, bool occlusion)
	{
		constexpr std::array labels{ "RR diffuse", "RR specular", "RR diffuse AO", "RR specular occlusion" };
		ImGui::PushID(id);
		for (uint32_t index = 0; index < (occlusion ? labels.size() : 2u); ++index) {
			const auto bit = 1u << index;
			bool enabled = (mask & bit) != 0;
			if (ImGui::Checkbox(labels[index], &enabled))
				mask = enabled ? mask | bit : mask & ~bit;
		}
		ImGui::PopID();
	}
}

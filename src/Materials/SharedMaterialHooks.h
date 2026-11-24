#pragma once

#include <array>
#include <optional>
#include <string>

namespace SharedMaterialHooks
{
	/** @brief Installs backend-independent form editor-ID and lighting pass hooks once. */
	void Install();
	/** @brief Reports whether the session uses the legacy NIF flag remapping hooks. */
	bool UsesLegacyNifMarkers();
	/** @brief Resolves a live texture set only when its record association is unambiguous. */
	std::string ResolveTextureSetEditorID(RE::BSTextureSet* textures);
	/** @brief Prevents exclusive mode from selecting legacy PBR shader permutations. */
	void NormalizeRasterDescriptors(RE::BSShader::Type type, uint32_t& vertex, uint32_t& pixel);
	/** @brief Obtains projected color metadata from the selected material authority. */
	std::optional<std::array<float, 3>> ProjectedColorScale(const RE::TESForm* form);
	struct LegacyResidencyStats { size_t meshes = 0, landscapes = 0; bool glints = false; };
	/** @brief Counts live legacy PBR extension records and the dedicated glint texture. */
	LegacyResidencyStats LegacyResidency();
}

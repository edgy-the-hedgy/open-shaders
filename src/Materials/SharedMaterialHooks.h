#pragma once

namespace SharedMaterialHooks
{
	/** @brief Installs backend-independent form editor-ID and lighting pass hooks once. */
	void Install();
	/** @brief Resolves a live texture set only when its record association is unambiguous. */
	std::string ResolveTextureSetEditorID(RE::BSTextureSet* textures);
}

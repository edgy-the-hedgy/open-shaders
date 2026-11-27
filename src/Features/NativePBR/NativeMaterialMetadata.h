#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace NativePBR
{
	/** @brief Installs native-only metadata hooks before the engine loads forms/NIFs. */
	void InstallExclusiveMetadataHooks();
	/** @brief Preserve original NIF flags without altering legacy material rendering. */
	void PreserveAuthoredFlags(RE::BSLightingShaderProperty& property);
	std::optional<uint64_t> ReadAuthoredFlags(const RE::BSLightingShaderProperty& property);
	/** @brief Bounded TXST conversion provenance, verified against current texture paths. */
	void RememberTextureSet(RE::BGSTextureSet* record, RE::BSTextureSet* converted);
	std::string ResolveTextureSet(RE::BSTextureSet* textures);
	void ReadNifParameters(const RE::BSLightingShaderProperty& property,
		std::optional<std::array<float, 4>>& thin, std::optional<std::array<float, 2>>& fuzz);
}

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include "PBRRecordData.h"

namespace SharedMaterialHooks
{
	/** @brief Installs backend-independent form editor-ID and lighting pass hooks once. */
	void Install();
	/** @brief Reports whether the session uses the legacy NIF flag remapping hooks. */
	bool UsesLegacyNifMarkers();
	/** @brief Returns preserved source flags for a normalized native NIF property. */
	std::optional<uint64_t> OriginalNativePbrFlags(const RE::BSLightingShaderProperty* property);
	/** @brief Resolves a live texture set only when its record association is unambiguous. */
	std::string ResolveTextureSetEditorID(RE::BSTextureSet* textures);
	/** @brief Classifies lighting properties without interpreting authored bits as legacy flags. */
	enum class MaterialClass : uint8_t { Unknown, Legacy, LegacyPbrMesh, LegacyPbrLandscape, NativePbrMesh };
	struct MaterialMetadata
	{
		uint64_t runtimeFlags = 0;
		std::optional<uint64_t> authoredPbrFlags;
		MaterialClass classification = MaterialClass::Unknown;
	};
	/** @brief Returns runtime flags and independently preserved authored PBR flags. */
	MaterialMetadata ClassifyMaterial(const RE::BSLightingShaderProperty* property);
	/** @brief Reports whether legacy PBR material rendering is available. */
	bool UsesLegacyMaterialRendering();
	/** @brief Resolves reference-local projected MATO identity without consulting TruePBR. */
	std::string ResolveMaterialObjectEditorID(const RE::TESObjectREFR* reference);
	/** @brief Returns an owned NativePBR TXST recipe, independent of legacy catalogs. */
	std::optional<PBRRecords::TextureSetData> ResolveNativeTextureSet(std::string_view editorId);
	/** @brief Returns an owned NativePBR MATO recipe, independent of legacy catalogs. */
	std::optional<PBRRecords::MaterialObjectData> ResolveNativeMaterialObject(std::string_view editorId);
	struct LandscapeClassification
	{
		std::optional<bool> authoredPbr;
		std::optional<bool> rasterPbr;
	};
	/** @brief Separates authored terrain classification from active raster material encoding. */
	LandscapeClassification ClassifyLandscape(const RE::TESObjectLAND* land);
	/** @brief Returns a known PBR displacement scale, or no value when unresolved. */
	std::optional<float> PbrDisplacementScale(const RE::BSLightingShaderProperty* property);
	/** @brief Prevents exclusive mode from selecting legacy PBR shader permutations. */
	void NormalizeRasterDescriptors(RE::BSShader::Type type, uint32_t& vertex, uint32_t& pixel, bool deferred = false);
	/** @brief Removes the native route marker before engine shader lookup. */
	void StripNativeRasterRoute(RE::BSShader::Type type, uint32_t& vertex, uint32_t& pixel);
	/** @brief Obtains projected color metadata from the selected material authority. */
	std::optional<std::array<float, 3>> ProjectedColorScale(const RE::TESForm* form);
	struct LegacyResidencyStats { size_t meshes = 0, landscapes = 0; bool glints = false; };
	/** @brief Counts live legacy PBR extension records and the dedicated glint texture. */
	LegacyResidencyStats LegacyResidency();
}

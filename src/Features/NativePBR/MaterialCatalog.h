#pragma once

#include "Materials/PBRRecordData.h"

namespace NativePBR
{
	struct MaterialCatalogSnapshot
	{
		std::unordered_map<std::string, PBRRecords::TextureSetData> textureSets;
		std::unordered_map<std::string, PBRRecords::MaterialObjectData> materialObjects;
		size_t rejectedTextureSets = 0;
		size_t rejectedMaterialObjects = 0;

		/** @brief Return a recipe while the caller retains this immutable snapshot. */
		const PBRRecords::TextureSetData* FindTextureSet(std::string_view a_editorId) const;
		/** @brief Return a MATO recipe while the caller retains this immutable snapshot. */
		const PBRRecords::MaterialObjectData* FindMaterialObject(std::string_view a_editorId) const;
	};

	class MaterialCatalog
	{
	public:
		/** @brief Read legacy-compatible PBR asset configs into NativePBR-owned storage. */
		void Load(bool a_verbose = false);
		/** @brief Obtain the current immutable material-source catalog. */
		std::shared_ptr<const MaterialCatalogSnapshot> GetSnapshot() const;

	private:
		std::atomic<std::shared_ptr<const MaterialCatalogSnapshot>> published;
	};
}

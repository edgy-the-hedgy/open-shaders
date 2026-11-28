#pragma once

#include "Features/NativePBR/GpuMaterialAbi.h"

namespace DX12 { struct PreparedMaterialSource; }
namespace NativePBR { struct MaterialCatalogSnapshot; }

namespace DX12Native
{
	struct SceneInputs;

	/** @brief Immutable CPU recipe candidates aligned with the exact geometry publication.
	 * No descriptor residency, raster suppression, or current-frame pixel ownership is implied.
	 */
	struct PreparedMaterialFrame
	{
		uint64_t geometryRevision = 0;
		uint32_t scope = 0;
		std::vector<NativePBR::Recipe> recipes;
		std::vector<NativePBR::GpuMaterialRecord> records;
		std::vector<NativePBR::GpuDrawBinding> bindings;
		std::vector<uint32_t> candidateIndices;  // Geometry ordinal -> CPU recipe, UINT32_MAX if absent.
		size_t sources = 0, missing = 0, unknownAuthored = 0, limited = 0;
		size_t resolvedTxst = 0, resolvedMato = 0;
	};

	/** @brief Compile copied, authored mesh metadata independently of geometry storage. */
	std::optional<NativePBR::Recipe> CompilePreparedMeshRecipe(const DX12::PreparedMaterialSource& source,
		uint64_t identity, uint64_t revision, const NativePBR::MaterialCatalogSnapshot* catalog,
		std::string_view materialObject = {}, size_t* resolvedTxst = nullptr, size_t* resolvedMato = nullptr, bool rasterLocal = false);
	std::shared_ptr<const PreparedMaterialFrame> CompilePreparedMaterialFrame(const SceneInputs& inputs);
}

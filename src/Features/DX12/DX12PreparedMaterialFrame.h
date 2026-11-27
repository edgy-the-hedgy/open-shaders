#pragma once

#include "Features/NativePBR/GpuMaterialAbi.h"

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

	std::shared_ptr<const PreparedMaterialFrame> CompilePreparedMaterialFrame(const SceneInputs& inputs);
}

#pragma once

#include "DX12NativeSceneInputs.h"
#include "Features/NativePBR/GpuMaterialAbi.h"
#include "Features/NativePBR/MaterialCatalog.h"
#include "Features/NativePBR/MaterialInterpretation.h"
#include <string_view>
#include <bit>
#include <format>

namespace DX12Native
{
	// Preliminary CPU material generation. No runtime TXST/MATO resolution,
	// texture residency or rendering authority is implied by its existence.
	struct PreparedMaterials
	{
		uint64_t sceneRevision = 0;
		std::vector<NativePBR::Recipe> recipes;
		std::vector<NativePBR::GpuDrawBinding> bindings;
		std::vector<NativePBR::GpuMaterialRecord> records;
		uint32_t missing = 0, unsupported = 0;
		uint32_t runtimeMissing = 0, runtimeSources = 0, companionMissing = 0, recoveredPlacements = 0;
		std::array<uint32_t, 5> rejected{};
	};

	inline std::shared_ptr<const PreparedMaterials> CompilePreparedMaterials(const SceneInputs& inputs, const std::shared_ptr<const NativePBR::MaterialCatalogSnapshot>& catalog)
	{
		auto result = std::make_shared<PreparedMaterials>();
		result->sceneRevision = inputs.geometryRevision;
		result->bindings.reserve(inputs.geometry.size());
		std::unordered_map<std::string, uint32_t> sources;
		std::unordered_set<uint64_t> identities;
		constexpr uint32_t pbrSource = 1u << 23;  // open-RT on-disk menu-screen flag.
		constexpr std::array<size_t, 7> slots{ 0, 1, 5, 2, 3, 7, 6 };
		const auto hash = [](std::string_view key) {
			uint64_t value = 14695981039346656037ull;
			for (const char ch : key) { value ^= static_cast<unsigned char>(ch); value *= 1099511628211ull; }
			return value ? value : 1;
		};
		for (const auto& instance : inputs.geometry) {
			const auto* source = instance.shape ? instance.shape->shading.get() : nullptr;
			const auto missingBinding = NativePBR::PackGpuDrawBinding(0, 0, UINT32_MAX, 0);
			if (!source) {
				++result->missing;
				if (instance.shape && instance.shape->runtimeObjectKey) ++result->runtimeMissing;
				else ++result->companionMissing;
				result->bindings.push_back(missingBinding);
				continue;
			}
			if (source->runtimeObserved) ++result->runtimeSources;
			if (source->recoveredPlacement) ++result->recoveredPlacements;
			const auto& placementMaterial = instance.shape->materialOverride;
			if ((placementMaterial && placementMaterial->ambiguous) || !(source->shaderFlags2 & pbrSource) || source->alpha != 1.0f ||
				(source->stride != 60 && source->stride != 36) || source->clampMode < 0 || source->uvTransform.size() != 4) {
				++result->unsupported;
				if (placementMaterial && placementMaterial->ambiguous) ++result->rejected[0];
				if (!(source->shaderFlags2 & pbrSource)) ++result->rejected[1];
				if (source->alpha != 1.0f) ++result->rejected[2];
				if (source->stride != 60 && source->stride != 36) ++result->rejected[3];
				if (source->clampMode < 0 || source->uvTransform.size() != 4) ++result->rejected[4];
				result->bindings.push_back(missingBinding);
				continue;
			}
			auto key = source->sourceNif + ":" + source->sourceHash + ":" + std::to_string(source->blockIndex);
			if (source->runtimeObserved) {
				key = std::format("runtime:{}:{}:{}", source->shaderFlags1, source->shaderFlags2, source->clampMode);
				for (float value : source->uvTransform) key += std::format(":{:08x}", std::bit_cast<uint32_t>(value));
				for (const auto& path : source->textures) key += std::format(":{}:{}", path.size(), path);
			}
			if (placementMaterial) {
				key += ":TXST:" + placementMaterial->textureSet + ":MATO:" + placementMaterial->materialObject;
				for (const auto& path : placementMaterial->textures) key += ":" + path;
			}
			auto it = sources.find(key);
			uint32_t index = 0;
			if (it == sources.end()) {
				const auto identity = hash(key);
				if (!identities.insert(identity).second) {
					++result->unsupported;
					result->bindings.push_back(missingBinding);
					continue;
				}
				NativePBR::MaterialData material;
				material.materialAlpha = source->alpha;
				material.clampMode = uint32_t(source->clampMode);
				std::copy_n(source->uvTransform.begin(), 4, material.uvOffsetScale[0].begin());
				material.uvOffsetScale[1] = material.uvOffsetScale[0];
				const auto& textures = placementMaterial && !placementMaterial->textures.empty() ? placementMaterial->textures : source->textures;
				for (size_t role = 0; role < slots.size(); ++role)
					if (slots[role] < textures.size()) material.texturePaths[role] = textures[slots[role]];
				if (placementMaterial && catalog) {
					if (const auto* config = catalog->FindTextureSet(std::string_view(placementMaterial->textureSet)))
						material = NativePBR::InterpretTextureSet(material, *config);
					if (const auto* config = catalog->FindMaterialObject(std::string_view(placementMaterial->materialObject))) {
						const auto projected = NativePBR::InterpretMaterialObject(placementMaterial->materialObject, *config);
						material.projectedColor = projected.baseColorScale;
						material.projectedRoughness = projected.roughness;
						material.projectedSpecularLevel = projected.specularLevel;
						material.projectedGlint = projected.glint;
					}
				}
				material.texturePathsComplete = true;
				auto recipe = NativePBR::CompileRecipe({ identity, 1, std::move(material) });
				NativePBR::GpuTextureLocations locations{};
				for (auto& location : locations)
					location = { NativePBR::kGpuMissingTexture, 0 };
				index = uint32_t(result->recipes.size());
				result->records.push_back(NativePBR::PackGpuMaterialRecord(recipe, locations));
				result->recipes.push_back(std::move(recipe));
				sources.emplace(key, index);
			} else {
				index = it->second;
			}
			const auto& recipe = result->recipes[index];
			// Retain the identity, but prohibit use as a resident GPU binding.
			result->bindings.push_back(NativePBR::PackGpuDrawBinding(recipe.identity, recipe.revision, UINT32_MAX, 0));
		}
		return result;
	}
}

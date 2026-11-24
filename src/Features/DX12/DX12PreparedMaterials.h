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
	// CPU recipes are immutable inputs; GPU residency and pixel ownership are separate.
	struct PreparedMaterials
	{
		uint64_t sceneRevision = 0;
		std::vector<NativePBR::Recipe> recipes;
		std::vector<NativePBR::GpuDrawBinding> bindings;
		std::vector<NativePBR::GpuMaterialRecord> records;
		uint32_t missing = 0, unsupported = 0;
		uint32_t runtimeMissing = 0, runtimeSources = 0, companionMissing = 0, recoveredPlacements = 0;
		std::array<uint32_t, 5> rejected{};
		std::array<uint32_t, size_t(DX12::RuntimeMaterialFailure::Count)> runtimeFailures{};
		uint32_t alphaTested = 0, resolvedTextureSets = 0, resolvedMaterialObjects = 0;
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
				if (instance.shape && instance.shape->runtimeObjectKey) {
					++result->runtimeMissing;
					++result->runtimeFailures[size_t(instance.shape->runtimeMaterialFailure)];
				}
				else ++result->companionMissing;
				result->bindings.push_back(missingBinding);
				continue;
			}
			if (source->runtimeObserved) ++result->runtimeSources;
			if (source->alphaTest) ++result->alphaTested;
			if (source->recoveredPlacement) ++result->recoveredPlacements;
			const auto& placementMaterial = instance.shape->materialOverride;
			const bool pbrIdentified = source->runtimeObserved || (source->shaderFlags2 & pbrSource);
			const std::string_view txst = !source->runtimeTextureSet.empty() ? std::string_view(source->runtimeTextureSet) :
				(placementMaterial ? std::string_view(placementMaterial->textureSet) : std::string_view{});
			const std::string_view mato = !source->runtimeMaterialObject.empty() ? std::string_view(source->runtimeMaterialObject) :
				(placementMaterial ? std::string_view(placementMaterial->materialObject) : std::string_view{});
			if (catalog && !txst.empty() && catalog->FindTextureSet(txst)) ++result->resolvedTextureSets;
			if (catalog && !mato.empty() && catalog->FindMaterialObject(mato)) ++result->resolvedMaterialObjects;
			if ((placementMaterial && placementMaterial->ambiguous) || !pbrIdentified || source->alpha != 1.0f ||
				(source->stride != 60 && source->stride != 36) || source->clampMode < 0 || source->uvTransform.size() != 4) {
				++result->unsupported;
				if (placementMaterial && placementMaterial->ambiguous) ++result->rejected[0];
				if (!pbrIdentified) ++result->rejected[1];
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
				if (source->nifThinSurface) {
					key += ":thin";
					for (float value : *source->nifThinSurface) key += std::format(":{:08x}", std::bit_cast<uint32_t>(value));
				}
				if (source->nifFuzz) {
					key += ":fuzz";
					for (float value : *source->nifFuzz) key += std::format(":{:08x}", std::bit_cast<uint32_t>(value));
				}
			}
			key += std::format(":TXST:{}:{}:MATO:{}:{}", txst.size(), txst, mato.size(), mato);
			if (placementMaterial) {
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
				if (!source->runtimeObserved || source->originalPbrFlags) {
					using F = RE::BSShaderProperty::EShaderPropertyFlag;
					const uint64_t flags = uint64_t(source->shaderFlags1) | (uint64_t(source->shaderFlags2) << 32);
					const auto has = [flags](F flag) { return (flags & static_cast<uint64_t>(flag)) != 0; };
					const auto add = [&](PBRFlags flag) { material.pbrFlags |= uint32_t(flag); };
					if (has(F::kMultiLayerParallax)) {
						add(PBRFlags::TwoLayer);
						material.coatStrength = 1.0f;
						if (has(F::kSoftLighting)) add(PBRFlags::InterlayerParallax);
						if (has(F::kBackLighting)) add(PBRFlags::CoatNormal);
						if (has(F::kEffectLighting)) add(PBRFlags::ColoredCoat);
					} else if (has(F::kBackLighting)) {
						add(PBRFlags::HairMarschner);
					} else {
						if (has(F::kAnisotropicLighting)) add(PBRFlags::ThinSurface);
						if (has(F::kEffectLighting)) add(PBRFlags::Foliage);
						material.nifThinSurfaceFlags = material.pbrFlags;
						if (has(F::kRimLighting)) add(PBRFlags::Subsurface);
						if (has(F::kSoftLighting)) add(PBRFlags::Fuzz);
						else if (has(F::kFitSlope)) material.glint.enabled = true;
					}
				}
				material.clampMode = uint32_t(source->clampMode);
				std::copy_n(source->uvTransform.begin(), 4, material.uvOffsetScale[0].begin());
				material.uvOffsetScale[1] = material.uvOffsetScale[0];
				const auto& textures = placementMaterial && !placementMaterial->textures.empty() ? placementMaterial->textures : source->textures;
				for (size_t role = 0; role < slots.size(); ++role)
					if (slots[role] < textures.size()) material.texturePaths[role] = textures[slots[role]];
				if (catalog) {
					if (const auto* config = catalog->FindTextureSet(txst))
						material = NativePBR::InterpretTextureSet(material, *config);
					if (const auto* config = catalog->FindMaterialObject(mato)) {
						const auto projected = NativePBR::InterpretMaterialObject(std::string(mato), *config);
						material.projectedColor = projected.baseColorScale;
						material.projectedRoughness = projected.roughness;
						material.projectedSpecularLevel = projected.specularLevel;
						material.projectedGlint = projected.glint;
					}
				}
				if (!(material.pbrFlags & (uint32_t(PBRFlags::TwoLayer) | uint32_t(PBRFlags::HairMarschner)))) {
					if (source->nifThinSurface) NativePBR::InterpretNifThinSurface(*source->nifThinSurface, material);
					if (source->nifFuzz) NativePBR::InterpretNifFuzz(*source->nifFuzz, material);
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

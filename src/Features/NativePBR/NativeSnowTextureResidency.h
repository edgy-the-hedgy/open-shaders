#pragma once

#include "GpuMaterialResources.h"
#include "NativeSnowPublication.h"
#include "Features/DX12NativeScene/MaterialTextureLookup.h"

namespace NativePBR
{
	/** @brief Resolves a verified DDS identity to a generation-local descriptor and array slice. */
	inline std::optional<std::array<uint32_t, 2>> FindSnowTextureLocation(
		const SnowTextureReference& a_texture,
		const std::unordered_map<std::string, uint64_t>& a_locations,
		const std::unordered_map<uint64_t, std::array<uint32_t, 2>>& a_logical,
		size_t a_poolCount)
	{
		if (a_texture.canonicalPath.empty() || !a_texture.srgb)
			return std::nullopt;
		const auto found = a_locations.find(DX12Native::MaterialTextureLookupKey(a_texture.canonicalPath));
		if (found == a_locations.end())
			return std::nullopt;
		const auto page = a_logical.find(found->second);
		if (page == a_logical.end() || page->second[0] >= a_poolCount)
			return std::nullopt;
		const auto descriptor = GpuTextureDescriptorIndex(page->second[0],
			*a_texture.srgb ? TextureInterpretation::ColorSrgb : TextureInterpretation::DataLinear);
		if (descriptor == kGpuMissingTexture)
			return std::nullopt;
		return std::array<uint32_t, 2>{ descriptor, page->second[1] };
	}

	/** @brief Reuses native DDS descriptors without creating a second texture-page allocation. */
	inline bool ResolveSnowTexture(SnowTextureReference& a_texture, const GpuMaterialResources& a_gpu)
	{
		if (a_texture.canonicalPath.empty() || !a_texture.srgb || !a_gpu.residency || !a_gpu.textureDescriptors)
			return false;
		const auto& pages = *a_gpu.residency;
		const auto location = FindSnowTextureLocation(a_texture, pages.locations, pages.logicalLocations, pages.pools.size());
		if (!location)
			return false;
		const auto physicalPool = *a_texture.srgb ? location->at(0) - kGpuTexturePoolCapacity : location->at(0);
		if (!pages.pools[physicalPool].resource)
			return false;
		a_texture.descriptorIndex = location->at(0);
		a_texture.sliceIndex = location->at(1);
		a_texture.resourceGeneration = a_gpu.generation ? a_gpu.generation->revision : 0;
		a_texture.status = SnowTextureStatus::Resident;
		return true;
	}

	/** @brief Publishes residency only for the same completed material generation as the snow inputs. */
	inline std::shared_ptr<const NativeSnowPublication> BindNativeSnowTextureResidency(
		std::shared_ptr<const NativeSnowPublication> a_source,
		std::shared_ptr<const GpuMaterialResources> a_gpu)
	{
		if (!a_source)
			return {};
		if (!a_gpu || !a_source->prepared || !a_source->prepared->materialPublication ||
			a_gpu->generation != a_source->prepared->materialPublication->recipes)
			return a_source;
		auto result = std::make_shared<NativeSnowPublication>(*a_source);
		result->gpuMaterials = std::move(a_gpu);
		result->projectedTexturesResolved = result->projectedTexturesUnresolved = 0;
		result->textureSetsResident = result->textureSetsPartial = result->textureSetsUnavailable = 0;
		for (auto& instance : result->instances) {
			if ((instance.classification & SnowProjected) == 0)
				continue;
			uint32_t resident = 0;
			for (auto& texture : instance.projection.textures) {
				if (ResolveSnowTexture(texture, *result->gpuMaterials))
					++resident;
				else if (texture.status == SnowTextureStatus::IdentifiedPath)
					texture.status = SnowTextureStatus::Unavailable;
			}
			result->projectedTexturesResolved += resident;
			result->projectedTexturesUnresolved += uint32_t(instance.projection.textures.size()) - resident;
			result->textureSetsResident += resident == instance.projection.textures.size();
			result->textureSetsPartial += resident != 0 && resident < instance.projection.textures.size();
			result->textureSetsUnavailable += resident == 0;
		}
		return result;
	}
}

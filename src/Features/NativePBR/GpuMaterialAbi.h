#pragma once

#include "NativePBRCompiler.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace NativePBR
{
	inline constexpr uint32_t kGpuMissingTexture = UINT32_MAX;
	inline constexpr uint32_t kGpuMaterialRecordVersion = 1;
	using GpuTextureLocation = std::array<uint32_t, 2>;
	using GpuTextureLocations = std::array<GpuTextureLocation, static_cast<size_t>(TextureRole::Count)>;

	/** @brief Geometry-independent SM6 material record, indexed by stable material identity. */
	struct GpuMaterialRecord
	{
		std::array<std::array<uint32_t, 4>, 4> textures{};
		std::array<std::array<float, 4>, 2> uv{};
		std::array<float, 4> base{};
		std::array<float, 4> coat{};
		std::array<float, 4> surface{};
		std::array<float, 4> projected{};
		std::array<float, 4> coatColor{};
		std::array<float, 4> subsurfaceColor{};
		std::array<float, 4> fuzzColor{};
		std::array<float, 4> thinSurface{};
		std::array<float, 4> glint{};
		std::array<float, 4> projectedGlint{};
		std::array<uint32_t, 4> properties{};
		std::array<uint32_t, 4> identity{};
		std::array<uint32_t, 4> controls{};
	};
	static_assert(sizeof(GpuMaterialRecord) == 304);
	static_assert(offsetof(GpuMaterialRecord, uv) == 64);
	static_assert(offsetof(GpuMaterialRecord, properties) == 256);
	static_assert(offsetof(GpuMaterialRecord, identity) == 272);
	static_assert(offsetof(GpuMaterialRecord, controls) == 288);

	/** @brief Packs all observed material features without treating unsupported lobes as residency failures. */
	inline GpuMaterialRecord PackGpuMaterialRecord(const Recipe& a_recipe, const GpuTextureLocations& a_locations)
	{
		GpuMaterialRecord result;
		uint32_t residentRoles = 0;
		for (size_t role = 0; role < a_locations.size(); ++role) {
			const auto& location = a_locations[role];
			auto& pair = result.textures[role / 2];
			pair[(role % 2) * 2] = location[0];
			pair[(role % 2) * 2 + 1] = location[1];
			if (location[0] != kGpuMissingTexture)
				residentRoles |= 1u << role;
		}
		result.textures[3][2] = kGpuMissingTexture;
		const auto& source = a_recipe.parameters;
		result.uv = source.uvOffsetScale;
		result.base = { source.materialAlpha, source.roughnessScale, source.displacementScale, source.specularLevel };
		result.coat = { source.coatStrength, source.coatRoughness, source.coatSpecularLevel, source.subsurfaceOpacity };
		result.surface = { source.fuzzWeight, source.subsurfaceBalance, source.fuzzRoughness, source.projectedRoughness };
		result.projected = { source.projectedColor[0], source.projectedColor[1], source.projectedColor[2], source.projectedSpecularLevel };
		result.coatColor = { source.coatColor[0], source.coatColor[1], source.coatColor[2], 0 };
		result.subsurfaceColor = { source.subsurfaceColor[0], source.subsurfaceColor[1], source.subsurfaceColor[2], 0 };
		result.fuzzColor = { source.fuzzColor[0], source.fuzzColor[1], source.fuzzColor[2], 0 };
		result.thinSurface = { source.thinSurfaceParameters[0], source.thinSurfaceParameters[1], source.thinSurfaceParameters[2], 0 };
		result.glint = { source.glint.screenScale, source.glint.microfacetDensity, source.glint.roughness, source.glint.randomness };
		result.projectedGlint = { source.projectedGlint.screenScale, source.projectedGlint.microfacetDensity, source.projectedGlint.roughness, source.projectedGlint.randomness };
		result.properties = { source.pbrFlags, source.nifThinSurfaceFlags, source.clampMode, a_recipe.issues };
		result.identity = { uint32_t(a_recipe.identity), uint32_t(a_recipe.identity >> 32),
			uint32_t(a_recipe.revision), uint32_t(a_recipe.revision >> 32) };
		result.controls = { residentRoles,
			(source.glint.enabled ? 1u : 0u) | (source.projectedGlint.enabled ? 2u : 0u) |
			(source.fuzzRoughnessTexture ? 4u : 0u), uint32_t(source.kind), 0u };
		return result;
	}
}

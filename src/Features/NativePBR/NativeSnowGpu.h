#pragma once

#include "GpuMaterialAbi.h"
#include "NativeSnowPublication.h"

#include <array>
#include <cstdint>

namespace NativePBR
{
	inline constexpr uint32_t kGpuSnowLayerEnabled = 1u;
	inline constexpr uint32_t kGpuSnowLayerColorOnly = 1u;
	inline constexpr uint32_t kGpuSnowLayerTextured = 2u;

	struct GpuSnowLayerRecord
	{
		std::array<std::array<uint32_t, 4>, 2> textures{
			std::array<uint32_t, 4>{ kGpuMissingTexture, 0, kGpuMissingTexture, 0 },
			std::array<uint32_t, 4>{ kGpuMissingTexture, 0, kGpuMissingTexture, 0 } };
		std::array<float, 4> projectedUVParams{};
		std::array<float, 4> projectedUVParams2{};
		std::array<float, 4> projectedUVParams3{};
		std::array<float, 4> materialRGBScale{};
		std::array<float, 4> directionAndSpace{};
		std::array<uint32_t, 4> flags{};
	};
	static_assert(sizeof(GpuSnowLayerRecord) == 128);

	inline std::optional<uint32_t> NativeSnowSamplerMode(const SnowTextureReference& a_texture)
	{
		if (!a_texture.sampler)
			return std::nullopt;
		const auto& sampler = *a_texture.sampler;
		if ((sampler.addressU != 1u && sampler.addressU != 3u) ||
			(sampler.addressV != 1u && sampler.addressV != 3u))
			return std::nullopt;
		return (sampler.addressU == 1u ? 2u : 0u) | (sampler.addressV == 1u ? 1u : 0u);
	}

	/** @brief Admits only fresh, matched, fully resident projected layers without changing base UVs. */
	inline GpuSnowLayerRecord PackNativeSnowLayer(const NativeSnowInstance& a_snow,
		uint32_t a_frame, bool a_vr, uint64_t a_materialGeneration)
	{
		GpuSnowLayerRecord result;
		if (a_vr || !a_materialGeneration || (a_snow.classification & SnowProjected) == 0 ||
			(a_snow.classification & SnowLandscape) != 0 || a_snow.source.conflictingPasses ||
			(a_snow.status != SnowInputStatus::Observed && a_snow.status != SnowInputStatus::NifOnly) ||
			!SnowProjectionReadyForFrame(a_snow.projection, a_frame, false))
			return result;
		const auto mode = GetSnowProjectedTextureMode(a_snow.projection);
		const uint32_t required = SnowProjectedRequiredTextureMask(mode);
		if (!required)
			return result;
		uint32_t samplerModes = 0;
		for (uint32_t role = 0; role < uint32_t(SnowTextureRole::Count); ++role) {
			if ((required & (1u << role)) == 0)
				continue;
			const auto& texture = a_snow.projection.textures[role];
			const auto sampler = NativeSnowSamplerMode(texture);
			if (texture.status != SnowTextureStatus::Resident || texture.descriptorIndex >= kGpuTextureDescriptorCapacity ||
				texture.resourceGeneration != a_materialGeneration || !sampler)
				return GpuSnowLayerRecord{};
			result.textures[role / 2][(role % 2) * 2] = texture.descriptorIndex;
			result.textures[role / 2][(role % 2) * 2 + 1] = texture.sliceIndex;
			samplerModes |= *sampler << (role * 2);
		}
		result.projectedUVParams = *a_snow.projection.projectedUVParams;
		result.projectedUVParams2 = *a_snow.projection.projectedUVParams2;
		result.projectedUVParams3 = *a_snow.projection.projectedUVParams3;
		result.materialRGBScale = *a_snow.projection.materialObjectRGBScale;
		const auto& direction = *a_snow.projection.direction;
		result.directionAndSpace = { direction[0], direction[1], direction[2],
			float(uint32_t(a_snow.projection.coordinateSpace)) };
		result.flags = { kGpuSnowLayerEnabled,
			mode == SnowProjectedTextureMode::Textured ? kGpuSnowLayerTextured : kGpuSnowLayerColorOnly,
			0u, samplerModes };
		return result;
	}

	inline uint32_t CountNativeSnowGpuReady(const NativeSnowPublication& a_publication,
		uint32_t a_frame, bool a_vr, uint64_t a_materialGeneration)
	{
		uint32_t count = 0;
		for (const auto& instance : a_publication.instances)
			count += (PackNativeSnowLayer(instance, a_frame, a_vr, a_materialGeneration).flags[0] &
				kGpuSnowLayerEnabled) != 0;
		return count;
	}
}

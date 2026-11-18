#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <array>
#include <string>

namespace NativePBR
{
	inline constexpr uint32_t kSnowShaderFlag = 1u << 21;
	inline constexpr uint32_t kBaseObjectSnowShaderFlag = 1u << 19;
	inline constexpr uint32_t kProjectedUvShaderFlag = 1u << 15;
	inline constexpr uint64_t kSnowNifFlag = 1ull << 60;
	inline constexpr uint64_t kProjectedUvNifFlag = 1ull << 23;
	inline constexpr uint64_t kLandscapeNifFlag = 1ull << 14;
	inline constexpr size_t kSnowProjectedTextureCount = 4;
	inline constexpr std::array<uint32_t, kSnowProjectedTextureCount> kSnowProjectedTextureSlots{ 3, 8, 10, 11 };

	/** @brief Copies the observed D3D11 sampler descriptor without coupling publication to device objects. */
	struct ObservedSnowSampler
	{
		uint32_t filter = 0, addressU = 0, addressV = 0, addressW = 0;
		uint32_t maxAnisotropy = 0, comparisonFunc = 0;
		float mipLODBias = 0, minLOD = 0, maxLOD = 0;
		std::array<float, 4> borderColor{};
		bool operator==(const ObservedSnowSampler&) const = default;
	};

	struct ObservedSnowTexture
	{
		std::string canonicalPath;
		std::optional<bool> srgb;
		std::optional<ObservedSnowSampler> sampler;
		bool operator==(const ObservedSnowTexture&) const = default;
	};

	enum SnowClassification : uint32_t
	{
		SnowOrdinary = 1u << 0,
		SnowBaseObject = 1u << 1,
		SnowProjected = 1u << 2,
		SnowLandscape = 1u << 3
	};

	/** @brief Keeps NIF state distinct from per-pass lighting flags before shader lookup normalization. */
	struct SnowSourceState
	{
		std::optional<uint64_t> nifFlags;
		std::optional<uint64_t> runtimeNifFlags;
		std::optional<uint32_t> renderPassDescriptor;
		std::array<ObservedSnowTexture, kSnowProjectedTextureCount> projectedTextures;
		bool conflictingPasses = false;
		bool operator==(const SnowSourceState&) const = default;
	};

	/** @brief Classifies only source flags that were actually observed. */
	constexpr uint32_t ClassifySnow(const SnowSourceState& a_source)
	{
		uint32_t result = 0;
		if (a_source.nifFlags) {
			if ((*a_source.nifFlags & kSnowNifFlag) != 0)
				result |= SnowOrdinary;
			if ((*a_source.nifFlags & kProjectedUvNifFlag) != 0)
				result |= SnowProjected;
			if ((*a_source.nifFlags & kLandscapeNifFlag) != 0)
				result |= SnowLandscape;
		}
		if (a_source.runtimeNifFlags) {
			if ((*a_source.runtimeNifFlags & kSnowNifFlag) != 0)
				result |= SnowOrdinary;
			if ((*a_source.runtimeNifFlags & kProjectedUvNifFlag) != 0)
				result |= SnowProjected;
			if ((*a_source.runtimeNifFlags & kLandscapeNifFlag) != 0)
				result |= SnowLandscape;
		}
		if (a_source.renderPassDescriptor) {
			const auto flags = *a_source.renderPassDescriptor;
			if ((flags & kSnowShaderFlag) != 0)
				result |= SnowOrdinary;
			if ((flags & kBaseObjectSnowShaderFlag) != 0)
				result |= SnowBaseObject;
			if ((flags & kProjectedUvShaderFlag) != 0)
				result |= SnowProjected;
		}
		return result;
	}

	/** @brief Comparing only snow flags prevents other lighting passes from creating false conflicts. */
	constexpr bool SnowPassesAgree(uint32_t a_first, uint32_t a_second)
	{
		constexpr uint32_t mask = kSnowShaderFlag | kBaseObjectSnowShaderFlag | kProjectedUvShaderFlag;
		return (a_first & mask) == (a_second & mask);
	}

	/** @brief Reports stale or mismatched NIF material state without inventing a scene match. */
	constexpr SnowSourceState BindSnowSource(uint64_t a_preparedNifFlags, SnowSourceState a_observed = {})
	{
		constexpr uint64_t mask = kSnowNifFlag | kProjectedUvNifFlag | kLandscapeNifFlag;
		if (a_observed.runtimeNifFlags &&
			((*a_observed.runtimeNifFlags ^ a_preparedNifFlags) & mask) != 0)
			a_observed.conflictingPasses = true;
		a_observed.nifFlags = a_preparedNifFlags;
		return a_observed;
	}
}

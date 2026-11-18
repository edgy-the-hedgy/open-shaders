#pragma once

#include "PreparedSceneMaterials.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NativePBR
{
	inline constexpr uint32_t kNativeSnowPublicationVersion = 1;

	enum class SnowInputStatus : uint8_t
	{
		Observed,
		NifOnly,
		MissingMaterial,
		AmbiguousInstance,
		UnresolvedProjection,
		LandscapeMetadataOnly
	};

	enum class SnowProjectionSpace : uint8_t { Unresolved, Object, World };
	enum class SnowTextureRole : uint8_t { Diffuse, Normal, DetailNormal, Noise, Count };
	enum class SnowTextureStatus : uint8_t { UnresolvedSource, IdentifiedPath, InvalidPath, Resident, Unavailable };

	/** @brief Explicit runtime texture identities; NIF diffuse/normal paths are not projected texture paths. */
	struct SnowTextureReference
	{
		SnowTextureStatus status = SnowTextureStatus::UnresolvedSource;
		std::string canonicalPath;
		uint32_t descriptorIndex = UINT32_MAX, sliceIndex = 0;
		uint64_t resourceGeneration = 0;
	};

	/** @brief Projection values remain absent until their originating engine constants are observed. */
	struct NativeSnowProjection
	{
		SnowProjectionSpace coordinateSpace = SnowProjectionSpace::Unresolved;
		std::optional<std::array<float, 3>> direction;
		std::optional<std::array<float, 12>> textureProjection;
		std::optional<std::array<float, 4>> projectedUVParams, projectedUVParams2, projectedUVParams3;
		std::optional<std::array<float, 4>> materialObjectRGBScale, parallaxOccData;
		std::array<SnowTextureReference, static_cast<size_t>(SnowTextureRole::Count)> textures;
	};

	/** @brief Per-layer terrain snow remains unknown until landscape shader inputs are captured. */
	struct NativeLandscapeSnow
	{
		std::array<std::optional<bool>, 6> layers;
		std::optional<float> snowMask;
		std::optional<std::array<float, 6>> blendWeights;
	};

	struct NativeSnowInstance
	{
		uint32_t sceneIndex = 0, classification = 0;
		uint64_t geometryIdentity = 0, materialIdentity = 0, materialRevision = 0, textureGeneration = 0;
		SnowSourceState source;
		SnowInputStatus status = SnowInputStatus::MissingMaterial;
		NativeSnowProjection projection;
		NativeLandscapeSnow landscape;
	};

	/** @brief Immutable snow data pinned to the exact native scene and material generation. */
	struct NativeSnowPublication
	{
		uint32_t version = kNativeSnowPublicationVersion, cellID = 0;
		uint64_t sceneRevision = 0, materialSerial = 0, instanceEpoch = 0;
		std::shared_ptr<const PreparedSceneMaterials> prepared;
		std::vector<NativeSnowInstance> instances;
		uint32_t sceneInstances = 0, ordinary = 0, baseObject = 0, projected = 0, landscape = 0;
		uint32_t observed = 0, nifOnly = 0, incompleteProjection = 0, ambiguous = 0, missing = 0;

		/** @brief Rejects stale scene, material and contract versions before native consumption. */
		bool Matches(const PreparedSceneMaterials& a_prepared) const
		{
			return version == kNativeSnowPublicationVersion && prepared.get() == &a_prepared &&
				cellID == a_prepared.cellID && sceneRevision == a_prepared.sceneRevision &&
				materialSerial == a_prepared.materialSerial && instanceEpoch == a_prepared.instanceEpoch;
		}
	};

	/** @brief Distinguishes known snow classification from missing projection semantics. */
	constexpr SnowInputStatus ClassifySnowInput(uint32_t a_classification, bool a_hasRuntimeDescriptor,
		bool a_ambiguous, bool a_materialAvailable)
	{
		if (a_ambiguous)
			return SnowInputStatus::AmbiguousInstance;
		if (!a_materialAvailable)
			return SnowInputStatus::MissingMaterial;
		if ((a_classification & SnowProjected) != 0)
			return SnowInputStatus::UnresolvedProjection;
		if ((a_classification & SnowLandscape) != 0)
			return SnowInputStatus::LandscapeMetadataOnly;
		return a_hasRuntimeDescriptor ? SnowInputStatus::Observed : SnowInputStatus::NifOnly;
	}

	/** @brief Produces bounded metadata without performing or admitting any native shading. */
	inline std::shared_ptr<const NativeSnowPublication> PrepareNativeSnowPublication(
		std::shared_ptr<const PreparedSceneMaterials> a_prepared)
	{
		if (!a_prepared || !a_prepared->geometry ||
			a_prepared->instances.size() != a_prepared->geometry->instances.size() ||
			a_prepared->instances.size() > DX12Native::ShadowBindings::kMaximumInstances)
			return {};
		auto result = std::make_shared<NativeSnowPublication>();
		result->version = kNativeSnowPublicationVersion;
		result->cellID = a_prepared->cellID;
		result->sceneRevision = a_prepared->sceneRevision;
		result->materialSerial = a_prepared->materialSerial;
		result->instanceEpoch = a_prepared->instanceEpoch;
		result->sceneInstances = uint32_t(a_prepared->instances.size());
		result->prepared = std::move(a_prepared);
		result->instances.reserve(result->sceneInstances);
		for (const auto& binding : result->prepared->instances) {
			if (binding.instanceIndex >= result->sceneInstances)
				return {};
			const auto& geometry = result->prepared->geometry->instances[binding.instanceIndex];
			if (geometry.identity != binding.geometryIdentity)
				return {};
			NativeSnowInstance snow;
			snow.sceneIndex = binding.instanceIndex;
			snow.geometryIdentity = binding.geometryIdentity;
			snow.source = binding.material.instanceProperties.snow;
			if (!snow.source.nifFlags && geometry.material.offlineDescription) {
				const auto& desc = *geometry.material.offlineDescription;
				const auto first = desc.find("shaderFlags1"), second = desc.find("shaderFlags2");
				if (first != desc.end() && second != desc.end() && first->is_number_unsigned() && second->is_number_unsigned())
					snow.source.nifFlags = (uint64_t(second->get<uint32_t>()) << 32) | first->get<uint32_t>();
			}
			snow.classification = ClassifySnow(snow.source);
			if (!snow.classification)
				continue;
			if (binding.material.recipe) {
				snow.materialIdentity = binding.material.recipe->identity;
				snow.materialRevision = binding.material.recipe->revision;
				snow.textureGeneration = binding.material.recipe->parameters.textureGeneration;
			}
			snow.status = ClassifySnowInput(snow.classification, snow.source.renderPassDescriptor.has_value(),
				snow.source.conflictingPasses || binding.material.status == MaterialLookupStatus::AmbiguousInstance,
				bool(binding.material));
			result->ordinary += (snow.classification & SnowOrdinary) != 0;
			result->baseObject += (snow.classification & SnowBaseObject) != 0;
			result->projected += (snow.classification & SnowProjected) != 0;
			result->landscape += (snow.classification & SnowLandscape) != 0;
			result->observed += snow.status == SnowInputStatus::Observed;
			result->nifOnly += snow.status == SnowInputStatus::NifOnly;
			result->incompleteProjection += snow.status == SnowInputStatus::UnresolvedProjection;
			result->ambiguous += snow.status == SnowInputStatus::AmbiguousInstance;
			result->missing += snow.status == SnowInputStatus::MissingMaterial;
			result->instances.push_back(std::move(snow));
		}
		return result;
	}
}

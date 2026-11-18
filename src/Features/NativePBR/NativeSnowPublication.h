#pragma once

#include "PreparedSceneMaterials.h"
#include "NativeSnowObservations.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NativePBR
{
	struct GpuMaterialResources;
	inline constexpr uint32_t kNativeSnowPublicationVersion = 2;

	enum class SnowInputStatus : uint8_t
	{
		Observed,
		NifOnly,
		MissingMaterial,
		AmbiguousInstance,
		UnresolvedProjection,
		LandscapeMetadataOnly
	};

	enum class SnowTextureRole : uint8_t { Diffuse, Normal, DetailNormal, Noise, Count };
	enum class SnowTextureStatus : uint8_t { UnresolvedSource, IdentifiedPath, InvalidPath, Resident, Unavailable };

	/** @brief Explicit runtime texture identities; NIF diffuse/normal paths are not projected texture paths. */
	struct SnowTextureReference
	{
		SnowTextureStatus status = SnowTextureStatus::UnresolvedSource;
		std::string canonicalPath;
		std::optional<bool> srgb;
		std::optional<ObservedSnowSampler> sampler;
		uint32_t textureSlot = UINT32_MAX, samplerSlot = UINT32_MAX;
		uint32_t descriptorIndex = UINT32_MAX, sliceIndex = 0;
		uint64_t resourceGeneration = 0;
	};

	/** @brief Projection values remain absent until their originating engine constants are observed. */
	struct NativeSnowProjection
	{
		SnowProjectionSpace coordinateSpace = SnowProjectionSpace::Unresolved;
		std::optional<std::array<float, 3>> direction;
		std::optional<std::array<float, 12>> textureProjection;
		std::optional<std::array<float, 12>> rightEyeTextureProjection;
		std::optional<std::array<float, 4>> projectedUVParams, projectedUVParams2, projectedUVParams3;
		std::optional<std::array<float, 4>> materialObjectRGBScale, parallaxOccData;
		uint32_t sourceFrame = 0;
		std::array<SnowTextureReference, static_cast<size_t>(SnowTextureRole::Count)> textures;
	};

	/** @brief A projected coverage input is complete only when its original shader values exist. */
	inline bool SnowProjectionCoverageReady(const NativeSnowProjection& a_projection)
	{
		return a_projection.coordinateSpace != SnowProjectionSpace::Unresolved &&
			a_projection.direction && a_projection.textureProjection && a_projection.projectedUVParams &&
			a_projection.projectedUVParams2 && a_projection.projectedUVParams3;
	}

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
		uint64_t observationRevision = 0, observationEpoch = 0;
		std::shared_ptr<const PreparedSceneMaterials> prepared;
		std::shared_ptr<const GpuMaterialResources> gpuMaterials;
		std::vector<NativeSnowInstance> instances;
		uint32_t sceneInstances = 0, ordinary = 0, baseObject = 0, projected = 0, landscape = 0;
		uint32_t observed = 0, nifOnly = 0, completeProjection = 0, incompleteProjection = 0;
		uint32_t missingProjectionDirection = 0, missingProjectionParameters = 0, ambiguous = 0, missing = 0;
		uint32_t projectedTexturesResolved = 0, projectedTexturesUnresolved = 0;
		uint32_t textureSetsResident = 0, textureSetsPartial = 0, textureSetsUnavailable = 0;

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
		bool a_ambiguous, bool a_materialAvailable, bool a_projectionReady = false)
	{
		if (a_ambiguous)
			return SnowInputStatus::AmbiguousInstance;
		if (!a_materialAvailable)
			return SnowInputStatus::MissingMaterial;
		if ((a_classification & SnowProjected) != 0 && !a_projectionReady)
			return SnowInputStatus::UnresolvedProjection;
		if ((a_classification & SnowLandscape) != 0)
			return SnowInputStatus::LandscapeMetadataOnly;
		return a_hasRuntimeDescriptor ? SnowInputStatus::Observed : SnowInputStatus::NifOnly;
	}

	/** @brief Produces bounded metadata without performing or admitting any native shading. */
	inline std::shared_ptr<const NativeSnowPublication> PrepareNativeSnowPublication(
		std::shared_ptr<const PreparedSceneMaterials> a_prepared,
		std::shared_ptr<const NativeSnowObservations::Snapshot> a_observations = {})
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
		result->observationRevision = a_observations ? a_observations->revision : 0;
		result->observationEpoch = a_observations ? a_observations->epoch : 0;
		result->sceneInstances = uint32_t(a_prepared->instances.size());
		result->prepared = std::move(a_prepared);
		result->instances.reserve(std::min(result->sceneInstances, 512u));
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
			if (a_observations && !geometry.preparedIdentity.referenceKey.empty() &&
				!geometry.preparedIdentity.shapeName.empty()) {
				const auto key = NativeSnowObservations::Key(geometry.preparedIdentity.referenceKey,
					geometry.preparedIdentity.shapeName);
				if (const auto found = a_observations->entries.find(key); found != a_observations->entries.end())
					snow.source = snow.source.nifFlags ? BindSnowSource(*snow.source.nifFlags, found->second) : found->second;
			}
			snow.classification = ClassifySnow(snow.source);
			if (!snow.classification)
				continue;
			if (snow.source.projection) {
				const auto& observed = *snow.source.projection;
				snow.projection.coordinateSpace = observed.coordinateSpace;
				snow.projection.direction = observed.direction;
				snow.projection.textureProjection = observed.textureProjection[0];
				snow.projection.rightEyeTextureProjection = observed.textureProjection[1];
				snow.projection.projectedUVParams = observed.projectedUVParams;
				snow.projection.projectedUVParams2 = observed.projectedUVParams2;
				snow.projection.projectedUVParams3 = observed.projectedUVParams3;
				snow.projection.materialObjectRGBScale = observed.materialObjectRGBScale;
				snow.projection.parallaxOccData = observed.parallaxOccData;
				snow.projection.sourceFrame = observed.frame;
			}
			for (size_t role = 0; role < snow.projection.textures.size(); ++role) {
				const auto& observed = snow.source.projectedTextures[role];
				auto& target = snow.projection.textures[role];
				target.textureSlot = target.samplerSlot = kSnowProjectedTextureSlots[role];
				target.sampler = observed.sampler;
				if (observed.canonicalPath.empty())
					continue;
				target.canonicalPath = observed.canonicalPath;
				target.srgb = observed.srgb;
				target.status = SnowTextureStatus::IdentifiedPath;
			}
			if (binding.material.recipe) {
				snow.materialIdentity = binding.material.recipe->identity;
				snow.materialRevision = binding.material.recipe->revision;
				snow.textureGeneration = binding.material.recipe->parameters.textureGeneration;
			}
			snow.status = ClassifySnowInput(snow.classification, snow.source.renderPassDescriptor.has_value(),
				snow.source.conflictingPasses || binding.material.status == MaterialLookupStatus::AmbiguousInstance,
				bool(binding.material), SnowProjectionCoverageReady(snow.projection));
			result->ordinary += (snow.classification & SnowOrdinary) != 0;
			result->baseObject += (snow.classification & SnowBaseObject) != 0;
			result->projected += (snow.classification & SnowProjected) != 0;
			if ((snow.classification & SnowProjected) != 0) {
				result->completeProjection += SnowProjectionCoverageReady(snow.projection);
				result->incompleteProjection += !SnowProjectionCoverageReady(snow.projection);
				result->missingProjectionDirection += !snow.projection.direction ||
					snow.projection.coordinateSpace == SnowProjectionSpace::Unresolved;
				result->missingProjectionParameters += !snow.projection.projectedUVParams ||
					!snow.projection.projectedUVParams2 || !snow.projection.projectedUVParams3;
				result->projectedTexturesUnresolved += uint32_t(snow.projection.textures.size());
				++result->textureSetsUnavailable;
			}
			result->landscape += (snow.classification & SnowLandscape) != 0;
			result->observed += snow.status == SnowInputStatus::Observed;
			result->nifOnly += snow.status == SnowInputStatus::NifOnly;
			result->ambiguous += snow.status == SnowInputStatus::AmbiguousInstance;
			result->missing += snow.status == SnowInputStatus::MissingMaterial;
			result->instances.push_back(std::move(snow));
		}
		return result;
	}
}

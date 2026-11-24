#pragma once

#include <cstdint>

namespace DX12Native::MaterialFrameContract
{
	inline constexpr uint32_t kMaterialAbiVersion = 1;
	inline constexpr uint32_t kMaterialStride = 32;
	inline constexpr uint32_t kLightingStride = 16;
	inline constexpr uint32_t kBaseOnlyBinding = 1u << 11;
	inline constexpr uint32_t kMissingBaseTexturesBinding = 1u << 12;

	enum class SurfaceStatus : int32_t
	{
		InvalidReceiver = 0,
		EvaluatedBase = 1,
		FeatureLimited = 2,
		MissingGeometry = -1,
		DepthMismatch = -2,
		EvaluationFailed = -3,
		UnsupportedMaterial = -4,
		MissingTextures = -5
	};

	/** @brief Base evaluation validity is not final-color ownership. */
	constexpr bool HasEvaluatedBase(SurfaceStatus status)
	{
		return status == SurfaceStatus::EvaluatedBase || status == SurfaceStatus::FeatureLimited;
	}

	enum class SurfaceCorrespondence { DepthOnly, ExactRasterIdentity };
	enum class LightingCoverage { SelectedPointLights, CompleteRasterReplacement };

	enum class SignalEncoding { MaterialModulated, Demodulated, Reconstructed, FinalLinearOpaqueColor, Visibility };
	enum class ReconstructionState { Raw, Spatial, Temporal };
	enum class SamplingLayout { FullResolution, Sparse };

	enum Contribution : uint32_t
	{
		DirectDiffuse = 1u << 0, DirectSpecular = 1u << 1,
		IndirectDiffuse = 1u << 2, IndirectSpecular = 1u << 3,
		Transmission = 1u << 4, Emission = 1u << 5,
		RemainingOpaque = 1u << 6, IndependentVisibility = 1u << 7
	};
	inline constexpr uint32_t kOpaqueRequired = DirectDiffuse | DirectSpecular | IndirectDiffuse |
		IndirectSpecular | Transmission | Emission | RemainingOpaque;
	inline constexpr uint32_t kKnownContributions = kOpaqueRequired | IndependentVisibility;
	inline constexpr uint32_t kOpaqueRadianceStride = 8;

	struct ContributionAdmission
	{
		uint32_t available = 0, valid = 0;
	};

	/** @brief A zero contribution still needs an explicit valid bit. */
	constexpr bool CompleteOpaque(ContributionAdmission admission)
	{
		return !(admission.available & ~kKnownContributions) &&
			!(admission.valid & ~admission.available) &&
			(admission.valid & kOpaqueRequired) == kOpaqueRequired;
	}

	enum class OpaqueOwner { Raster, Native };
	constexpr OpaqueOwner ResolveOpaqueOwner(SurfaceStatus status, SurfaceCorrespondence correspondence,
		ContributionAdmission admission, bool coherent, bool earlyOpaqueStage, bool independentShadowActive)
	{
		return coherent && earlyOpaqueStage && !independentShadowActive &&
			status == SurfaceStatus::EvaluatedBase && correspondence == SurfaceCorrespondence::ExactRasterIdentity &&
			CompleteOpaque(admission) ? OpaqueOwner::Native : OpaqueOwner::Raster;
	}

	struct PixelJitter
	{
		float x = 0, y = 0;
		bool valid = false;
		constexpr bool operator==(const PixelJitter&) const = default;
	};

	/** @brief Actual jittered minus unjittered pixel position, +x right / +y down. */
	constexpr PixelJitter JitterFromClipOffset(float x, float y, uint32_t width, uint32_t height)
	{
		return { x * float(width) * 0.5f, -y * float(height) * 0.5f, width != 0 && height != 0 };
	}

	struct TemporalIdentity
	{
		uint32_t nativeFrame = 0, pluginFrame = 0, width = 0, height = 0, scope = 0;
		uint64_t historyRevision = 0;
		bool reverseDepth = false, resetHistory = true;
		uint64_t geometryRevision = 0, materialRevision = 0;
		PixelJitter jitter{}, previousJitter{};

		constexpr bool SameFrame(const TemporalIdentity& other) const
		{
			return nativeFrame == other.nativeFrame && pluginFrame == other.pluginFrame &&
				width && height && width == other.width && height == other.height &&
				scope == other.scope && historyRevision == other.historyRevision && reverseDepth == other.reverseDepth &&
				resetHistory == other.resetHistory && geometryRevision == other.geometryRevision && materialRevision == other.materialRevision &&
				jitter == other.jitter && previousJitter == other.previousJitter;
		}
		constexpr bool CanReuseHistory(const TemporalIdentity& previous) const
		{
			return !resetHistory && nativeFrame - previous.nativeFrame == 1u &&
				pluginFrame - previous.pluginFrame == 1u && width && height && width == previous.width &&
				height == previous.height && scope && scope == previous.scope &&
				historyRevision == previous.historyRevision && reverseDepth == previous.reverseDepth &&
				geometryRevision == previous.geometryRevision && materialRevision == previous.materialRevision &&
				jitter.valid && previous.jitter.valid;
		}
	};

	/** @brief Replacement requires exact raster correspondence and complete lighting. */
	constexpr bool CanReplaceRaster(SurfaceStatus status, SurfaceCorrespondence correspondence, LightingCoverage coverage)
	{
		return status == SurfaceStatus::EvaluatedBase &&
			correspondence == SurfaceCorrespondence::ExactRasterIdentity &&
			coverage == LightingCoverage::CompleteRasterReplacement;
	}

	/** @brief A submitted fence may be incomplete, but must have a valid signal value. */
	constexpr bool ValidProducerSignal(uint64_t value, uint64_t completed)
	{
		return value != 0 && value != UINT64_MAX && completed != UINT64_MAX;
	}

	/** @brief An untracked submission never retires on a normal completion value. */
	constexpr bool FenceRetired(uint64_t value, uint64_t completed)
	{
		return !value || completed == UINT64_MAX || (value != UINT64_MAX && completed >= value);
	}

	/** @brief Checks the packed surface ABI before a consumer binds it. */
	constexpr bool ValidMaterialABI(uint32_t version, uint32_t stride)
	{
		return version == kMaterialAbiVersion && stride == kMaterialStride;
	}

	/** @brief Rejects invalid dimensions, unaligned pitches and ByteAddressBuffer offset overflow. */
	constexpr bool ValidLayout(uint32_t width, uint32_t height, uint32_t pitch, uint32_t stride)
	{
		return width && height && stride && pitch % stride == 0 &&
			uint64_t(pitch) >= uint64_t(width) * stride &&
			uint64_t(pitch) * height <= UINT32_MAX;
	}
}

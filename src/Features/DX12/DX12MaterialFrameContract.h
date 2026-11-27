#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace DX12Native::MaterialFrameContract
{
	inline constexpr uint32_t kMaterialAbiVersion = 1;
	inline constexpr uint32_t kMaterialStride = 32;
	inline constexpr uint32_t kRayIdentityStride = 8;
	inline constexpr uint32_t kMaterialLobeStride = 32;
	inline constexpr uint32_t kParallaxShadowStride = 32;
	inline constexpr uint32_t kCoatReadyBinding = 1u << 13;
	inline constexpr uint32_t kSubsurfaceReadyBinding = 1u << 14;
	inline constexpr uint32_t kThinSurfaceReadyBinding = 1u << 15;
	inline constexpr uint32_t kParallaxReadyBinding = 1u << 4;
	inline constexpr uint32_t kFuzzReadyBinding = 1u << 5;
	inline constexpr uint32_t kFoliageReadyBinding = 1u << 6;
	inline constexpr uint32_t kLightingStride = 24;
	// Transmission is independently evaluated by both lighting producers.
	constexpr bool CompleteTransmission(bool directValid, bool indirectValid) { return directValid && indirectValid; }
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

	/** @brief Raster and ray-hit identities must refer to the same immutable generation.
	 * A draw observation is not a per-pixel stamp. Zero/invalid ordinals never confer ownership.
	 */
	struct RasterIdentityStamp
	{
		uint32_t frame = 0, scope = 0;
		uint64_t geometryRevision = 0, materialRevision = 0;
		constexpr bool operator==(const RasterIdentityStamp&) const = default;
	};

	struct RasterPixelIdentity
	{
		uint32_t instance = UINT32_MAX, primitive = UINT32_MAX;
	};
	static_assert(sizeof(RasterPixelIdentity) == kRayIdentityStride);
	inline constexpr uint32_t kMaxRasterPrimitiveToken = 65535;

	constexpr uint32_t RasterTokenBase(uint32_t used, uint32_t count)
	{
		return count && used <= kMaxRasterPrimitiveToken && count <= kMaxRasterPrimitiveToken - used ? used + 1u : 0u;
	}

	constexpr bool ValidRayIdentity(RasterPixelIdentity identity)
	{
		return identity.instance != UINT32_MAX && identity.primitive != UINT32_MAX;
	}

	// Spatial filtering may cross a triangle edge within one rigid instance.
	// Temporal history requires the same triangle within the same generation.
	constexpr bool SameNativeReceiver(RasterPixelIdentity a, RasterPixelIdentity b, bool temporal)
	{
		return ValidRayIdentity(a) && ValidRayIdentity(b) && a.instance == b.instance &&
			(!temporal || a.primitive == b.primitive);
	}

	constexpr bool MatchesRasterPixel(const RasterIdentityStamp& raster, const RasterIdentityStamp& ray,
		RasterPixelIdentity pixel, RasterPixelIdentity hit, uint32_t instanceCount, uint32_t primitiveCount)
	{
		return raster.scope && raster.geometryRevision && raster.materialRevision && raster == ray &&
			pixel.instance < instanceCount && pixel.primitive < primitiveCount &&
			pixel.instance == hit.instance && pixel.primitive == hit.primitive;
	}

	/** @brief Raw per-draw projector property values; not yet a shader projection contract.
	 *  Engine setup may transform them or select different textures. Never infer ownership from this record.
	 */
	struct ProjectedPropertyValues
	{
		std::array<float, 4> params{}, color{};
		bool operator==(const ProjectedPropertyValues&) const = default;
		bool Valid() const
		{
			for (const auto value : params) if (!std::isfinite(value)) return false;
			for (const auto value : color) if (!std::isfinite(value)) return false;
			return true;
		}
	};
	struct ProjectedPropertyObservation
	{
		ProjectedPropertyValues values{};
		bool observed = false, projected = false, conflicted = false;
		void Observe(bool active, const ProjectedPropertyValues& next = {})
		{
			if (conflicted) return;
			if ((active && !next.Valid()) ||
				(observed && (projected != active || (active && values != next)))) {
				conflicted = true;
				return;
			}
			observed = true;
			projected = active;
			if (active) values = next;
		}
		bool ValidProjection() const { return observed && projected && !conflicted; }
		bool RejectRasterCandidate() const { return conflicted || projected; }
	};

	/** @brief A unique attached draw match; duplicate matches are ambiguous even if their geometry is equivalent. */
	struct UniqueRasterCandidate
	{
		uint32_t ordinal = UINT32_MAX;
		bool ambiguous = false;
		constexpr void Observe(uint32_t value)
		{
			if (value == UINT32_MAX || ambiguous)
				return;
			if (ordinal == UINT32_MAX)
				ordinal = value;
			else if (ordinal != value) {
				ordinal = UINT32_MAX;
				ambiguous = true;
			}
		}
		constexpr bool Valid() const { return !ambiguous && ordinal != UINT32_MAX; }
	};

	enum class SurfaceCorrespondence { DepthOnly, ExactRasterIdentity };
	enum class LightingCoverage { SelectedPointLights, CompleteRasterReplacement, EnvironmentOnly, SelectedDirectLights };

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
	// Fully evaluated opaque base materials have no active transmission lobe;
	// their evaluated emission is a separate source, not a fabricated lighting lobe.
	inline constexpr uint32_t kBaseMaterialContributions = Emission | Transmission;
	inline constexpr uint32_t kOpaqueRequired = DirectDiffuse | DirectSpecular | IndirectDiffuse |
		IndirectSpecular | Transmission | Emission | RemainingOpaque;
	inline constexpr uint32_t kKnownContributions = kOpaqueRequired | IndependentVisibility;
	inline constexpr uint32_t kOpaqueRadianceStride = 8;

	constexpr uint32_t BaseMaterialValidContributions(SurfaceStatus status, bool emissionFinite, bool opaqueNonTransmitting)
	{
		if (status != SurfaceStatus::EvaluatedBase)
			return 0u;
		return (emissionFinite ? uint32_t(Emission) : 0u) |
			(opaqueNonTransmitting ? uint32_t(Transmission) : 0u);
	}

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
	inline constexpr uint32_t kRasterIdentityCandidate = 1u << 0;
	inline constexpr uint32_t kExactRasterWinner = 1u << 1;
	inline constexpr uint32_t kCompleteOpaqueRadiance = 1u << 2;
	inline constexpr uint32_t kNativeOwnsOpaque = 1u << 3;
	// A matching diagnostic MRT token is deliberately not exact winner certification.
	constexpr SurfaceCorrespondence CertifyRasterCorrespondence(bool candidateMatch, bool allRasterWritersCertified)
	{
		return candidateMatch && allRasterWritersCertified ? SurfaceCorrespondence::ExactRasterIdentity : SurfaceCorrespondence::DepthOnly;
	}
	constexpr OpaqueOwner ResolveOpaqueOwner(SurfaceStatus status, SurfaceCorrespondence correspondence,
		ContributionAdmission admission, bool coherent, bool earlyOpaqueStage, bool independentShadowActive)
	{
		return coherent && earlyOpaqueStage && !independentShadowActive &&
			status == SurfaceStatus::EvaluatedBase && correspondence == SurfaceCorrespondence::ExactRasterIdentity &&
			CompleteOpaque(admission) ? OpaqueOwner::Native : OpaqueOwner::Raster;
	}

	/** @brief A candidate mask is evidence, not permission. Only the explicit owner
	 * bit may be consumed by a final-color replacement or a raster suppression path.
	 */
	constexpr uint32_t ResolveOwnershipBits(bool candidate, bool certifiedWinner,
		bool completeLightingCoverage, SurfaceStatus status, ContributionAdmission admission,
		bool coherent, bool earlyOpaqueStage, bool independentShadowActive)
	{
		const auto correspondence = CertifyRasterCorrespondence(candidate, certifiedWinner);
		const bool complete = completeLightingCoverage && CompleteOpaque(admission);
		uint32_t bits = candidate ? kRasterIdentityCandidate : 0u;
		if (correspondence == SurfaceCorrespondence::ExactRasterIdentity)
			bits |= kExactRasterWinner;
		if (complete)
			bits |= kCompleteOpaqueRadiance;
		if (complete && ResolveOpaqueOwner(status, correspondence, admission,
				coherent, earlyOpaqueStage, independentShadowActive) == OpaqueOwner::Native)
			bits |= kNativeOwnsOpaque;
		return bits;
	}

	constexpr bool NativeOwnsOpaque(uint32_t bits)
	{
		constexpr uint32_t required = kRasterIdentityCandidate | kExactRasterWinner |
			kCompleteOpaqueRadiance | kNativeOwnsOpaque;
		return (bits & required) == required && !(bits & ~required);
	}

	/** @brief Color application additionally requires the final-color encoding
	 * contract; a complete ownership mask cannot authorize the wrong color space.
	 */
	constexpr bool CanApplyFinalOpaque(uint32_t bits, bool sameFrame, bool encodingCompatible, bool earlyOpaqueStage)
	{
		return NativeOwnsOpaque(bits) && sameFrame && encodingCompatible && earlyOpaqueStage;
	}

	/** @brief Conservative same-pixel history, not motion-vector reprojection. */
	struct StationaryHistoryIdentity
	{
		uint32_t frame = 0, pluginFrame = 0, width = 0, height = 0, scope = 0, available = 0;
		uint64_t geometryRevision = 0, materialRevision = 0, environmentRevision = 0;
		bool reset = true;
		constexpr bool CanReuse(const StationaryHistoryIdentity& previous) const
		{
			return !reset && frame - previous.frame == 1u && pluginFrame - previous.pluginFrame == 1u &&
				width && height && width == previous.width && height == previous.height &&
				scope && scope == previous.scope && available && available == previous.available &&
				geometryRevision == previous.geometryRevision && materialRevision == previous.materialRevision &&
				environmentRevision == previous.environmentRevision;
		}
	};

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

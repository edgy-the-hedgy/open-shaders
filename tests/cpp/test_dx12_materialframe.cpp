#include "Features/DX12/DX12MaterialFrameContract.h"
#include "../../package/Shaders/Common/NativeShadowRT.hlsli"
#include "Features/NativePBR/GpuMaterialAbi.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace DX12Native::MaterialFrameContract;

TEST_CASE("Native material frames admit aligned full-resolution layouts", "[dx12][materials]")
{
	REQUIRE(ValidLayout(1920, 1080, 1920 * kMaterialStride, kMaterialStride));
	REQUIRE(ValidLayout(1921, 1080, 1928 * kMaterialStride, kMaterialStride));
	REQUIRE(ValidLayout(3840, 2160, 3840 * kLightingStride, kLightingStride));
}

TEST_CASE("Native material frames reject incomplete or overflowing layouts", "[dx12][materials]")
{
	REQUIRE_FALSE(ValidLayout(0, 1080, 1920 * kMaterialStride, kMaterialStride));
	REQUIRE_FALSE(ValidLayout(1920, 0, 1920 * kMaterialStride, kMaterialStride));
	REQUIRE_FALSE(ValidLayout(1920, 1080, 0, 0));
	REQUIRE_FALSE(ValidLayout(1921, 1080, 1920 * kMaterialStride, kMaterialStride));
	REQUIRE_FALSE(ValidLayout(1920, 1080, 1920 * kMaterialStride + 1, kMaterialStride));
	REQUIRE_FALSE(ValidLayout(16384, 16384, 16384 * kMaterialStride, kMaterialStride));
}

TEST_CASE("Native material publications reject incompatible ABI and unsignaled fences", "[dx12][materials]")
{
	REQUIRE(ValidMaterialABI(kMaterialAbiVersion, kMaterialStride));
	REQUIRE_FALSE(ValidMaterialABI(kMaterialAbiVersion + 1, kMaterialStride));
	REQUIRE_FALSE(ValidMaterialABI(kMaterialAbiVersion, kLightingStride));
	REQUIRE(ValidProducerSignal(5, 0));
	REQUIRE(ValidProducerSignal(5, 5));
	REQUIRE_FALSE(ValidProducerSignal(0, 0));
	REQUIRE_FALSE(ValidProducerSignal(UINT64_MAX, 5));
	REQUIRE_FALSE(ValidProducerSignal(5, UINT64_MAX));
}

TEST_CASE("Native material feature demand does not change base readiness", "[dx12][materials]")
{
	using namespace NativePBR;
	Recipe recipe{};
	const auto set = [&](TextureRole role) {
		recipe.textures[static_cast<size_t>(role)].canonicalPath = "textures/test.dds";
	};
	set(TextureRole::Emission);
	set(TextureRole::Displacement);
	set(TextureRole::Features0);
	set(TextureRole::Features1);
	recipe.parameters.displacementScale = 0.0f;
	REQUIRE(GpuBaseRequestedTextureRoles(recipe) == (kGpuRequiredTextureRoles | TextureRoleBit(TextureRole::Emission)));
	REQUIRE(GpuSpecializedTextureRoles(recipe) == 0u);

	recipe.parameters.pbrFlags = uint32_t(PBRFlags::Subsurface);
	REQUIRE(GpuSpecializedTextureRoles(recipe) == TextureRoleBit(TextureRole::Features0));

	recipe.parameters.pbrFlags = uint32_t(PBRFlags::Fuzz);
	recipe.parameters.fuzzWeight = 1.0f;
	recipe.parameters.fuzzRoughnessTexture = true;
	REQUIRE(GpuSpecializedTextureRoles(recipe) == (TextureRoleBit(TextureRole::Features1) | TextureRoleBit(TextureRole::Displacement)));
	recipe.parameters.fuzzWeight = 0.0f;
	REQUIRE(GpuSpecializedTextureRoles(recipe) == 0u);

	recipe.parameters.pbrFlags = uint32_t(PBRFlags::TwoLayer);
	recipe.parameters.coatStrength = 1.0f;
	REQUIRE(GpuSpecializedTextureRoles(recipe) == (TextureRoleBit(TextureRole::Features0) | TextureRoleBit(TextureRole::Features1)));
	REQUIRE(GpuBaseRequestedTextureRoles(recipe) == (kGpuRequiredTextureRoles | TextureRoleBit(TextureRole::Emission)));
	recipe.parameters.displacementScale = 1.0f;
	REQUIRE((GpuSpecializedTextureRoles(recipe) & TextureRoleBit(TextureRole::Displacement)) != 0u);
}

TEST_CASE("Evaluated native base is distinct from raster replacement eligibility", "[dx12][materials]")
{
	REQUIRE(HasEvaluatedBase(SurfaceStatus::EvaluatedBase));
	REQUIRE(HasEvaluatedBase(SurfaceStatus::FeatureLimited));
	for (auto status : { SurfaceStatus::InvalidReceiver, SurfaceStatus::MissingGeometry, SurfaceStatus::DepthMismatch,
		SurfaceStatus::EvaluationFailed, SurfaceStatus::UnsupportedMaterial, SurfaceStatus::MissingTextures }) {
		REQUIRE_FALSE(HasEvaluatedBase(status));
		REQUIRE_FALSE(CanReplaceRaster(status, SurfaceCorrespondence::ExactRasterIdentity, LightingCoverage::CompleteRasterReplacement));
	}
	REQUIRE_FALSE(CanReplaceRaster(SurfaceStatus::FeatureLimited, SurfaceCorrespondence::ExactRasterIdentity, LightingCoverage::CompleteRasterReplacement));
	REQUIRE_FALSE(CanReplaceRaster(SurfaceStatus::EvaluatedBase, SurfaceCorrespondence::DepthOnly, LightingCoverage::CompleteRasterReplacement));
	REQUIRE_FALSE(CanReplaceRaster(SurfaceStatus::EvaluatedBase, SurfaceCorrespondence::ExactRasterIdentity, LightingCoverage::SelectedPointLights));
	REQUIRE(CanReplaceRaster(SurfaceStatus::EvaluatedBase, SurfaceCorrespondence::ExactRasterIdentity, LightingCoverage::CompleteRasterReplacement));
}

TEST_CASE("Opaque ownership requires every contribution and exact identity", "[dx12][materials]")
{
	const auto exact = SurfaceCorrespondence::ExactRasterIdentity;
	const auto base = SurfaceStatus::EvaluatedBase;
	const ContributionAdmission full{ kOpaqueRequired, kOpaqueRequired };
	REQUIRE(ResolveOpaqueOwner(base, exact, full, true, true, false) == OpaqueOwner::Native);
	for (uint32_t bit = 1; bit <= RemainingOpaque; bit <<= 1) {
		REQUIRE_FALSE(CompleteOpaque({ kOpaqueRequired, kOpaqueRequired & ~bit }));
		REQUIRE(ResolveOpaqueOwner(base, exact, { kOpaqueRequired, kOpaqueRequired & ~bit }, true, true, false) == OpaqueOwner::Raster);
	}
	REQUIRE_FALSE(CompleteOpaque({ DirectDiffuse, kOpaqueRequired }));
	REQUIRE(ResolveOpaqueOwner(base, SurfaceCorrespondence::DepthOnly, full, true, true, false) == OpaqueOwner::Raster);
	REQUIRE(ResolveOpaqueOwner(SurfaceStatus::FeatureLimited, exact, full, true, true, false) == OpaqueOwner::Raster);
	REQUIRE(ResolveOpaqueOwner(base, exact, full, false, true, false) == OpaqueOwner::Raster);
	REQUIRE(ResolveOpaqueOwner(base, exact, full, true, false, false) == OpaqueOwner::Raster);
	REQUIRE(ResolveOpaqueOwner(base, exact, full, true, true, true) == OpaqueOwner::Raster);
}

TEST_CASE("Temporal identity rejects gaps, extent changes and resets", "[dx12][materials]")
{
	TemporalIdentity previous{ 5, 100, 1920, 1080, 7, 3, true, true };
	TemporalIdentity now{ 6, 101, 1920, 1080, 7, 3, true, false };
	previous.jitter = JitterFromClipOffset(1.0f / 1920.0f, -1.0f / 1080.0f, 1920, 1080);
	now.jitter = JitterFromClipOffset(0.0f, 0.0f, 1920, 1080);
	now.previousJitter = previous.jitter;
	REQUIRE(previous.jitter.valid);
	REQUIRE(previous.jitter.x > 0.49999f);
	REQUIRE(previous.jitter.x < 0.50001f);
	REQUIRE(previous.jitter.y > 0.49999f);
	REQUIRE(previous.jitter.y < 0.50001f);
	REQUIRE(now.CanReuseHistory(previous));
	REQUIRE(now.SameFrame(now));
	REQUIRE_FALSE(now.SameFrame(previous));
	now.resetHistory = true;
	REQUIRE_FALSE(now.CanReuseHistory(previous));
	now.resetHistory = false;
	++now.nativeFrame;
	REQUIRE_FALSE(now.CanReuseHistory(previous));
	--now.nativeFrame;
	++now.width;
	REQUIRE_FALSE(now.CanReuseHistory(previous));
	--now.width;
	++now.scope;
	REQUIRE_FALSE(now.CanReuseHistory(previous));
	--now.scope;
	++now.materialRevision;
	REQUIRE_FALSE(now.CanReuseHistory(previous));
	--now.materialRevision;
	now.jitter.valid = false;
	REQUIRE_FALSE(now.CanReuseHistory(previous));
}

TEST_CASE("Fence retirement never treats an untracked submission as complete", "[dx12][materials]")
{
	REQUIRE(FenceRetired(0, 0));
	REQUIRE_FALSE(FenceRetired(UINT64_MAX, 100));
	REQUIRE_FALSE(FenceRetired(5, 4));
	REQUIRE(FenceRetired(5, 5));
	REQUIRE(FenceRetired(UINT64_MAX, UINT64_MAX));
	REQUIRE_FALSE(ValidProducerSignal(5, UINT64_MAX));
}

TEST_CASE("Red-only native texture bindings require height-only data", "[dx12][materials]")
{
	using namespace NativePBR;
	Recipe recipe{};
	for (size_t i = 0; i < static_cast<size_t>(TextureRole::Count); ++i) {
		const auto role = static_cast<TextureRole>(i);
		REQUIRE(GpuAcceptsRedOnlyTexture(recipe, role) == (role == TextureRole::Displacement));
	}
	recipe.parameters.pbrFlags = uint32_t(PBRFlags::Fuzz);
	recipe.parameters.fuzzWeight = 1.0f;
	recipe.parameters.fuzzRoughnessTexture = true;
	REQUIRE_FALSE(GpuAcceptsRedOnlyTexture(recipe, TextureRole::Displacement));
	recipe.parameters.fuzzRoughnessTexture = false;
	REQUIRE(GpuAcceptsRedOnlyTexture(recipe, TextureRole::Displacement));
	recipe.parameters.fuzzRoughnessTexture = true;
	recipe.parameters.fuzzWeight = 0.0f;
	REQUIRE(GpuAcceptsRedOnlyTexture(recipe, TextureRole::Displacement));
}

TEST_CASE("Full Native stationary history requires uninterrupted coherent publications", "[dx12][materials]")
{
	StationaryHistoryIdentity previous{ 5, 8, 1920, 1080, 12, DirectDiffuse, 7, 7, 4, true, 2 };
	auto now = previous;
	now.frame++;
	now.pluginFrame++;
	now.reset = false;
	REQUIRE(now.CanReuse(previous));
	now.reset = true;
	REQUIRE_FALSE(now.CanReuse(previous));
	now.reset = false;
	REQUIRE(now.SameEnvironmentContent(previous));
	now.environmentRevision++;
	REQUIRE(now.CanReuse(previous));
	REQUIRE(now.CanReuseIndirect(previous));
	REQUIRE_FALSE(now.SameEnvironmentContent(previous));
	now.environmentEpoch++;
	REQUIRE(now.CanReuse(previous));
	REQUIRE_FALSE(now.CanReuseIndirect(previous));
	now.environmentEpoch--;
	now.environmentRevision--;
	now.available |= IndirectDiffuse;
	REQUIRE(now.CanReuse(previous));
	now.available = previous.available;
	now.screenSpaceReflections = true;
	REQUIRE(now.CanReuse(previous));
	REQUIRE(now.CanReuseIndirect(previous));
	REQUIRE_FALSE(now.SameEnvironmentContent(previous));
	now.screenSpaceReflections = false;
	now.available = IndirectDiffuse;
	REQUIRE_FALSE(now.CanReuse(previous));
	now.available = previous.available;
	now.frame++;
	REQUIRE_FALSE(now.CanReuse(previous));
	now.frame--;
	now.materialRevision++;
	REQUIRE_FALSE(now.CanReuse(previous));
}

TEST_CASE("Raster identity requires exact frame, generation, instance and primitive", "[dx12][materials]")
{
	RasterIdentityStamp stamp{ 42, 7, 11, 13 };
	const RasterPixelIdentity pixel{ 2, 5 };
	REQUIRE(MatchesRasterPixel(stamp, stamp, pixel, pixel, 3, 6));
	REQUIRE_FALSE(MatchesRasterPixel(stamp, stamp, {}, pixel, 3, 6));
	REQUIRE_FALSE(MatchesRasterPixel(stamp, stamp, pixel, { 1, 5 }, 3, 6));
	REQUIRE_FALSE(MatchesRasterPixel(stamp, stamp, pixel, { 2, 4 }, 3, 6));
	REQUIRE_FALSE(MatchesRasterPixel(stamp, stamp, pixel, pixel, 2, 6));
	REQUIRE_FALSE(MatchesRasterPixel(stamp, stamp, pixel, pixel, 3, 5));
	auto other = stamp;
	++other.frame;
	REQUIRE_FALSE(MatchesRasterPixel(stamp, other, pixel, pixel, 3, 6));
	other = stamp;
	++other.materialRevision;
	REQUIRE_FALSE(MatchesRasterPixel(stamp, other, pixel, pixel, 3, 6));
	other = stamp;
	other.scope = 0;
	REQUIRE_FALSE(MatchesRasterPixel(other, other, pixel, pixel, 3, 6));
}

TEST_CASE("Duplicate native draw candidates cannot claim a unique instance ordinal", "[dx12][materials]")
{
	UniqueRasterCandidate match;
	REQUIRE_FALSE(match.Valid());
	match.Observe(3);
	REQUIRE(match.Valid());
	match.Observe(3);
	REQUIRE(match.Valid());
	match.Observe(4);
	REQUIRE_FALSE(match.Valid());
	match.Observe(3);
	REQUIRE_FALSE(match.Valid());
}

TEST_CASE("Native ray identity has an independent bounded row layout", "[dx12][materials]")
{
	REQUIRE(ValidLayout(1921, 1080, 1928 * kRayIdentityStride, kRayIdentityStride));
	REQUIRE_FALSE(ValidLayout(1921, 1080, 1920 * kRayIdentityStride, kRayIdentityStride));
	REQUIRE_FALSE(ValidLayout(1921, 1080, 1928 * kRayIdentityStride + 1, kRayIdentityStride));
	REQUIRE_FALSE(ValidLayout(16384, 65536, 16384 * kRayIdentityStride, kRayIdentityStride));
}

TEST_CASE("Full Native reconstruction rejects cross-instance history and invalid hits", "[dx12][materials]")
{
	const RasterPixelIdentity a{ 4, 12 };
	REQUIRE(SameNativeReceiver(a, a, true));
	REQUIRE(SameNativeReceiver(a, { 4, 13 }, false));
	REQUIRE_FALSE(SameNativeReceiver(a, { 4, 13 }, true));
	REQUIRE_FALSE(SameNativeReceiver(a, { 5, 12 }, false));
	REQUIRE_FALSE(SameNativeReceiver(a, {}, false));
	REQUIRE_FALSE(SameNativeReceiver({ 4, UINT32_MAX }, a, true));
}

TEST_CASE("Full Native integer identities are not limited to a 16-bit token range", "[dx12][materials]")
{
	constexpr RasterPixelIdentity a{ 100000u, 1000000u };
	constexpr RasterPixelIdentity b{ 100000u, 1000001u };
	REQUIRE(ValidRayIdentity(a));
	REQUIRE(ValidRayIdentity(b));
	REQUIRE_FALSE(SameNativeReceiver(a, b, true));
	REQUIRE(SameNativeReceiver(a, b, false));
	REQUIRE_FALSE(ValidRayIdentity({ UINT32_MAX, 0 }));
}

TEST_CASE("A matching raster identity cannot independently grant opaque ownership", "[dx12][materials]")
{
	constexpr ContributionAdmission complete{ kOpaqueRequired, kOpaqueRequired };
	REQUIRE(ResolveOpaqueOwner(SurfaceStatus::EvaluatedBase, CertifyRasterCorrespondence(true, false), complete, true, true, false) == OpaqueOwner::Raster);
	REQUIRE(ResolveOpaqueOwner(SurfaceStatus::EvaluatedBase, CertifyRasterCorrespondence(false, true), complete, true, true, false) == OpaqueOwner::Raster);
	REQUIRE(ResolveOpaqueOwner(SurfaceStatus::EvaluatedBase, CertifyRasterCorrespondence(true, true), complete, true, true, false) == OpaqueOwner::Native);
}

TEST_CASE("Full Native owner bits require certified winner and complete encoded contributions", "[dx12][materials]")
{
	constexpr ContributionAdmission complete{ kOpaqueRequired, kOpaqueRequired };
	constexpr ContributionAdmission partial{ DirectDiffuse | DirectSpecular, DirectDiffuse | DirectSpecular };
	const auto resolve = [&](bool match, bool certified, bool coverage, ContributionAdmission admission,
		SurfaceStatus status = SurfaceStatus::EvaluatedBase, bool coherent = true,
		bool early = true, bool independentShadow = false) {
		return ResolveOwnershipBits(match, certified, coverage, status, admission, coherent, early, independentShadow);
	};
	REQUIRE(resolve(true, false, false, partial) == kRasterIdentityCandidate);
	REQUIRE_FALSE(NativeOwnsOpaque(resolve(true, true, false, complete)));
	REQUIRE_FALSE(NativeOwnsOpaque(resolve(true, true, true, partial)));
	REQUIRE_FALSE(NativeOwnsOpaque(resolve(true, true, true, complete, SurfaceStatus::FeatureLimited)));
	REQUIRE_FALSE(NativeOwnsOpaque(resolve(true, true, true, complete, SurfaceStatus::EvaluatedBase, false)));
	REQUIRE_FALSE(NativeOwnsOpaque(resolve(true, true, true, complete, SurfaceStatus::EvaluatedBase, true, false)));
	REQUIRE_FALSE(NativeOwnsOpaque(resolve(true, true, true, complete, SurfaceStatus::EvaluatedBase, true, true, true)));
	REQUIRE(NativeOwnsOpaque(resolve(true, true, true, complete)));
	REQUIRE_FALSE(NativeOwnsOpaque(kNativeOwnsOpaque));
	REQUIRE_FALSE(NativeOwnsOpaque(UINT32_MAX));
}

TEST_CASE("Full Native final color rejects stale ownership or incompatible encoding", "[dx12][materials]")
{
	constexpr uint32_t owned = kRasterIdentityCandidate | kExactRasterWinner | kCompleteOpaqueRadiance | kNativeOwnsOpaque;
	REQUIRE(CanApplyFinalOpaque(owned, true, true, true));
	REQUIRE_FALSE(CanApplyFinalOpaque(kRasterIdentityCandidate, true, true, true));
	REQUIRE_FALSE(CanApplyFinalOpaque(owned, false, true, true));
	REQUIRE_FALSE(CanApplyFinalOpaque(owned, true, false, true));
	REQUIRE_FALSE(CanApplyFinalOpaque(owned, true, true, false));
}

TEST_CASE("Base material contributions never imply complete opaque radiance", "[dx12][materials]")
{
	REQUIRE(BaseMaterialValidContributions(SurfaceStatus::EvaluatedBase, true, true) == kBaseMaterialContributions);
	REQUIRE(BaseMaterialValidContributions(SurfaceStatus::FeatureLimited, true, true) == 0u);
	REQUIRE(BaseMaterialValidContributions(SurfaceStatus::EvaluatedBase, false, true) == Transmission);
	REQUIRE(BaseMaterialValidContributions(SurfaceStatus::EvaluatedBase, true, false) == Emission);
	constexpr uint32_t partial = DirectDiffuse | DirectSpecular | IndirectDiffuse | IndirectSpecular | kBaseMaterialContributions;
	REQUIRE_FALSE(CompleteOpaque({ partial, partial }));
}

TEST_CASE("Native transmission requires both producer signals", "[dx12][materialframe]")
{
	using namespace DX12Native::MaterialFrameContract;
	STATIC_REQUIRE(kLightingStride == 24);
	STATIC_REQUIRE(CompleteTransmission(true,true));
	STATIC_REQUIRE_FALSE(CompleteTransmission(true,false));
	STATIC_REQUIRE_FALSE(CompleteTransmission(false,true));
	STATIC_REQUIRE_FALSE(CompleteTransmission(false,false));
}

TEST_CASE("Live projected properties reject conflicting observations and raster admission", "[dx12][materials]")
{
	ProjectedPropertyValues values{ { 1.0f, 0.5f, 0.25f, 0.0f }, { 0.7f, 0.8f, 0.9f, 1.0f } };
	ProjectedPropertyObservation observation;
	REQUIRE_FALSE(observation.ValidProjection());
	observation.Observe(true, values);
	REQUIRE(observation.ValidProjection());
	REQUIRE(observation.RejectRasterCandidate());
	observation.Observe(true, values);
	REQUIRE(observation.ValidProjection());
	values.params[0] = 2.0f;
	observation.Observe(true, values);
	REQUIRE_FALSE(observation.ValidProjection());
	REQUIRE(observation.conflicted);
	observation.Observe(true, values);
	REQUIRE_FALSE(observation.ValidProjection());
	ProjectedPropertyObservation toggled;
	toggled.Observe(false);
	REQUIRE_FALSE(toggled.RejectRasterCandidate());
	toggled.Observe(true, values);
	REQUIRE(toggled.RejectRasterCandidate());
	REQUIRE_FALSE(toggled.ValidProjection());
	ProjectedPropertyObservation invalid;
	values.params[0] = std::numeric_limits<float>::quiet_NaN();
	invalid.Observe(true, values);
	REQUIRE_FALSE(invalid.ValidProjection());
	REQUIRE(invalid.RejectRasterCandidate());
}

TEST_CASE("Projection constant extents reject invalid buffer ranges", "[dx12][materials]")
{
	STATIC_REQUIRE(ValidProjectionConstantExtent(160, 48, 208));
	STATIC_REQUIRE(ValidProjectionConstantExtent(96, 16, 112));
	STATIC_REQUIRE_FALSE(ValidProjectionConstantExtent(96, 16, 100));
	STATIC_REQUIRE_FALSE(ValidProjectionConstantExtent(95, 16, 112));
	STATIC_REQUIRE_FALSE(ValidProjectionConstantExtent(96, 12, 112));
	STATIC_REQUIRE_FALSE(ValidProjectionConstantExtent(UINT32_MAX, 16, 112));
}

TEST_CASE("Full Native sparse lighting matches RT Shadows dispatch parity at odd edges", "[dx12][lighting]")
{
	using namespace DX12Native;
	for (const auto quality : { kShadowQualityQuality, kShadowQualityPerformance }) {
		for (uint32_t width : { 1u, 2u, 3u, 17u, 32u }) {
			for (uint32_t height : { 1u, 2u, 5u, 16u }) {
				const auto lw = ShadowLogicalExtent(width, quality), lh = ShadowLogicalExtent(height, quality);
				for (uint32_t phase : { 0u, 1u }) {
					uint32_t expected = 0, actual = 0;
					for (uint32_t y = 0; y < lh; ++y)
						for (uint32_t x = 0; x < ShadowPhysicalWidth(lw, quality); ++x) {
							const auto lx = ShadowLogicalX(x, y, phase);
							if (lx >= lw) continue;
							++expected;
							REQUIRE(ShadowIsSampled(ShadowRenderCoordinate(lx, width, quality), ShadowRenderCoordinate(y, height, quality), width, height, quality, phase));
						}
					for (uint32_t y = 0; y < height; ++y)
						for (uint32_t x = 0; x < width; ++x) {
							const bool sampled = ShadowIsSampled(x, y, width, height, quality, phase);
							actual += sampled;
							REQUIRE_FALSE(sampled && ShadowIsSampled(x, y, width, height, quality, phase ^ 1u));
						}
					REQUIRE(actual == expected);
				}
			}
		}
	}
	REQUIRE(ShadowTemporalFrames(kShadowQualityQuality) == 2u);
	REQUIRE(ShadowTemporalFrames(kShadowQualityPerformance) == 4u);
	REQUIRE(ShadowReconstructionRadius(kShadowQualityQuality) == 1u);
	REQUIRE(ShadowReconstructionRadius(kShadowQualityPerformance) == 2u);
}

TEST_CASE("Native temporal lookup adds raster jitter delta once", "[dx12][materials]")
{
	const auto current = JitterFromClipOffset(0.5f / 960.0f, -0.5f / 540.0f, 1920, 1080);
	const auto previous = JitterFromClipOffset(-0.25f / 960.0f, 0.25f / 540.0f, 1920, 1080);
	const auto delta = HistoryJitterDelta(current, previous);
	REQUIRE(delta.valid);
	REQUIRE(delta.x > -0.751f);
	REQUIRE(delta.x < -0.749f);
	REQUIRE(delta.y > -0.751f);
	REQUIRE(delta.y < -0.749f);
	REQUIRE_FALSE(HistoryJitterDelta({}, previous).valid);
}

TEST_CASE("Raster-derived material identity can filter but cannot certify a ray hit", "[dx12][materials]")
{
	using namespace DX12Native::MaterialFrameContract;
	const RasterPixelIdentity ray{3u, 42u};
	const RasterPixelIdentity candidate{3u, 42u | kRasterDerivedPrimitive};
	const RasterIdentityStamp stamp{1u, 2u, 3u, 4u};
	REQUIRE(IsRasterDerived(candidate));
	REQUIRE(SameNativeReceiver(ray, candidate, true));
	REQUIRE(MatchesRasterPixel(stamp, stamp, ray, ray, 5u, 100u));
	REQUIRE_FALSE(MatchesRasterPixel(stamp, stamp, ray, candidate, 5u, 100u));
}

TEST_CASE("Character reflection geometry requires complete posed and primitive streams", "[dx12][materials]")
{
	STATIC_REQUIRE(ValidCharacterPrimitiveLayout(3, 1, 1, 216, 16));
	STATIC_REQUIRE_FALSE(ValidCharacterPrimitiveLayout(3, 1, 1, 215, 16));
	STATIC_REQUIRE_FALSE(ValidCharacterPrimitiveLayout(3, 1, 1, 216, 12));
	STATIC_REQUIRE_FALSE(ValidCharacterPrimitiveLayout(3, 1, 0, 216, 16));
	STATIC_REQUIRE_FALSE(ValidCharacterPrimitiveLayout(UINT32_MAX, UINT32_MAX, 1, UINT32_MAX, UINT32_MAX));
}

TEST_CASE("Projection transfer preserves physical BC mip extents", "[dx12][materials]")
{
	using namespace DX12Native::MaterialFrameContract;
	STATIC_REQUIRE(ProjectionTransferExtent(2048u, 0u, true) == 512u);
	STATIC_REQUIRE(ProjectionTransferExtent(2048u, 9u, true) == 1u);
	STATIC_REQUIRE(ProjectionTransferExtent(2048u, 10u, true) == 1u);
	STATIC_REQUIRE(ProjectionTransferExtent(2048u, 11u, true) == 1u);
	STATIC_REQUIRE(ProjectionTransferExtent(20u, 1u, true) == 3u);
	STATIC_REQUIRE(ProjectionTransferExtent(20u, 1u, false) == 10u);
	STATIC_REQUIRE(ProjectionTransferExtent(0u, 0u, true) == 0u);
	STATIC_REQUIRE(ProjectionTransferExtent(2048u, 32u, true) == 0u);
	STATIC_REQUIRE_FALSE(ProjectionRetryDue(699u, 100u, 600u));
	STATIC_REQUIRE(ProjectionRetryDue(700u, 100u, 600u));
	STATIC_REQUIRE(ProjectionRetryDue(0u, UINT32_MAX, 1u));
}

TEST_CASE("Raster publication survives scene generation changes without stale correspondence", "[dx12][materials]")
{
	const RasterIdentityStamp raster{42,7,100,200};
	REQUIRE(MatchesRasterPublication(raster,{42,7,101,201}));
	REQUIRE_FALSE(MatchesRasterScene(raster,{42,7,101,201}));
	REQUIRE(MatchesRasterScene(raster,raster));
	REQUIRE_FALSE(MatchesRasterScene({42,7,0,0},{42,7,0,0}));
	REQUIRE_FALSE(MatchesRasterPublication(raster,{43,7,100,200}));
	REQUIRE_FALSE(MatchesRasterPublication(raster,{42,8,100,200}));
	REQUIRE(RasterDrawCountForConsumer(17,true,false)==(kRasterLocalOnly|17u));
	REQUIRE(RasterDrawCountForConsumer(17,true,true)==17u);
	REQUIRE(RasterDrawCountForConsumer(17,false,false)==0u);
	REQUIRE(RasterDrawCountForConsumer(kRasterLocalOnly,true,true)==0u);
}

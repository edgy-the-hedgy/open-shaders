#include "Features/DX12/DX12MaterialFrameContract.h"
#include "Features/NativePBR/GpuMaterialAbi.h"

#include <catch2/catch_test_macros.hpp>

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
	StationaryHistoryIdentity previous{ 5, 8, 1920, 1080, 12, DirectDiffuse, 7, 7, 4, true };
	auto now = previous;
	now.frame++;
	now.pluginFrame++;
	now.reset = false;
	REQUIRE(now.CanReuse(previous));
	now.reset = true;
	REQUIRE_FALSE(now.CanReuse(previous));
	now.reset = false;
	now.environmentRevision++;
	REQUIRE_FALSE(now.CanReuse(previous));
	now.environmentRevision--;
	now.available |= IndirectDiffuse;
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

TEST_CASE("Full Native raster primitive tokens never alias on exhaustion", "[dx12][materials]")
{
	REQUIRE(RasterTokenBase(0, 1) == 1);
	REQUIRE(RasterTokenBase(65534, 1) == 65535);
	REQUIRE(RasterTokenBase(65534, 2) == 0);
	REQUIRE(RasterTokenBase(65535, 1) == 0);
	REQUIRE(RasterTokenBase(0, 0) == 0);
	REQUIRE(RasterTokenBase(UINT32_MAX, 1) == 0);
}

TEST_CASE("A matching raster token cannot independently grant opaque ownership", "[dx12][materials]")
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

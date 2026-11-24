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

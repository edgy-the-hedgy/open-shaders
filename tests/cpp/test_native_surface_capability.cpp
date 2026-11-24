#include "Features/NativePBR/NativePBRCompiler.h"

#include <catch2/catch_test_macros.hpp>

using namespace NativePBR;

namespace
{
	MaterialData BaseMaterial()
	{
		MaterialData material;
		material.texturePathsComplete = true;
		material.texturePaths[0] = "base.dds";
		material.texturePaths[1] = "normal.dds";
		material.texturePaths[2] = "rmaos.dds";
		return material;
	}
	Recipe MakeRecipe(const MaterialData& material)
	{
		return CompileRecipe({ 1, 1, material });
	}
}

TEST_CASE("Screen material defaults and inactive projection are not feature limits", "[dx12][materials]")
{
	auto material = BaseMaterial();
	REQUIRE(ClassifyScreenSurface(MakeRecipe(material), false).issues == 0);
	material.projectedRoughness = 0.5f;
	material.projectedGlint.enabled = true;
	const auto recipe = MakeRecipe(material);
	REQUIRE((recipe.issues & UnsupportedProjection) != 0);
	REQUIRE(ClassifyScreenSurface(recipe, false).issues == 0);
	REQUIRE(ClassifyScreenSurface(recipe, false).inactiveProjectedRecipe);
	const auto unresolved = ClassifyScreenSurface(recipe, false, true, false);
	REQUIRE(unresolved.issues == ScreenUnknownMetadata);
	REQUIRE(unresolved.unresolvedProjection);
	REQUIRE_FALSE(unresolved.inactiveProjectedRecipe);
	REQUIRE(ClassifyScreenSurface(recipe, true).issues == (ScreenProjection | ScreenGlint));
	REQUIRE(ClassifyScreenSurface(MakeRecipe(BaseMaterial()), true).issues == ScreenProjection);
	REQUIRE(ClassifyScreenSurface(MakeRecipe(BaseMaterial()), false, false).issues == ScreenUnknownMetadata);
}

TEST_CASE("Screen material never assumes parallax or coat evaluation exists", "[dx12][materials]")
{
	auto material = BaseMaterial();
	material.texturePaths[size_t(TextureRole::Displacement)] = "height.dds";
	auto recipe = MakeRecipe(material);
	REQUIRE(SupportsOpaqueParallax(recipe));
	REQUIRE(ClassifyScreenSurface(recipe, false).issues == ScreenDisplacement);
	material.displacementScale = 0.0f;
	REQUIRE(ClassifyScreenSurface(MakeRecipe(material), false).issues == ScreenDisplacement);
	material = BaseMaterial();
	material.pbrFlags = uint32_t(PBRFlags::TwoLayer);
	material.coatStrength = 1.0f;
	recipe = MakeRecipe(material);
	REQUIRE(SupportsOpaqueClearcoat(recipe));
	REQUIRE(ClassifyScreenSurface(recipe, false).issues == ScreenPbrFeatures);
	material.texturePaths[size_t(TextureRole::Features0)] = "coat.dds";
	REQUIRE(ClassifyScreenSurface(MakeRecipe(material), false).issues == (ScreenPbrFeatures | ScreenFeatureTextures));
}

TEST_CASE("Inactive lobes need resolved parameters rather than defaults", "[dx12][materials]")
{
	auto material = BaseMaterial();
	material.pbrFlags = uint32_t(PBRFlags::Fuzz);
	material.texturePaths[size_t(TextureRole::Features1)] = "fuzz.dds";
	material.glint.enabled = true;
	REQUIRE(ClassifyScreenSurface(MakeRecipe(material), false).issues == (ScreenPbrFeatures | ScreenFeatureTextures));
	material.textureSetParametersResolved = true;
	REQUIRE(ClassifyScreenSurface(MakeRecipe(material), false).issues == 0);
	material.fuzzWeight = 0.25f;
	REQUIRE(ClassifyScreenSurface(MakeRecipe(material), false).issues == (ScreenPbrFeatures | ScreenFeatureTextures));
	material = BaseMaterial();
	material.pbrFlags = uint32_t(PBRFlags::Subsurface);
	material.textureSetParametersResolved = true;
	material.subsurfaceOpacity = 1.0f;
	REQUIRE(ClassifyScreenSurface(MakeRecipe(material), false).issues == 0);
	material.texturePaths[size_t(TextureRole::Features0)] = "sss.dds";
	REQUIRE(ClassifyScreenSurface(MakeRecipe(material), false).issues == (ScreenPbrFeatures | ScreenFeatureTextures));
	material = BaseMaterial();
	material.pbrFlags = uint32_t(PBRFlags::TwoLayer) | uint32_t(PBRFlags::ThinSurface);
	material.textureSetParametersResolved = true;
	material.coatStrength = 0.0f;
	REQUIRE(ClassifyScreenSurface(MakeRecipe(material), false).issues == 0);
	material.pbrFlags |= uint32_t(PBRFlags::InterlayerParallax);
	REQUIRE(ClassifyScreenSurface(MakeRecipe(material), false).issues == ScreenPbrFeatures);
}

TEST_CASE("Base-only reason statistics count bindings without double counting totals", "[dx12][materials]")
{
	auto material = BaseMaterial();
	material.texturePaths[size_t(TextureRole::Displacement)] = "height.dds";
	const auto recipe = MakeRecipe(material);
	ScreenSurfaceTally tally;
	tally.Observe(recipe, ClassifyScreenSurface(recipe, false), false);
	tally.Observe(recipe, ClassifyScreenSurface(recipe, true), true);
	const auto base = MakeRecipe(BaseMaterial());
	tally.Observe(base, ClassifyScreenSurface(base, false), false);
	REQUIRE(tally.resident == 3);
	REQUIRE(tally.limited == 2);
	REQUIRE(tally.overlapping == 1);
	REQUIRE(tally.reasons[2] == 2);
	REQUIRE(tally.soleReasons[2] == 1);
	REQUIRE(tally.reasons[3] == 1);
	REQUIRE(tally.soleReasons[3] == 0);
}

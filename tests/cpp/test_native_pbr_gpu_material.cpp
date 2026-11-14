#include "Features/NativePBR/GpuMaterialAbi.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("NativePBR GPU material ABI preserves all seven texture roles", "[NativePBR][gpu-material]")
{
	NativePBR::Recipe recipe;
	recipe.identity = 0x1122334455667788ull;
	recipe.revision = 0x99aabbccddeeff00ull;
	recipe.issues = NativePBR::UnsupportedDisplacement | NativePBR::UnsupportedFeatureTextures;
	recipe.parameters.roughnessScale = 0.67f;
	recipe.parameters.displacementScale = 1.7f;
	recipe.parameters.projectedColor = { 0.2f, 0.3f, 0.4f };
	recipe.parameters.glint.enabled = true;
	NativePBR::GpuTextureLocations locations{};
	for (size_t role = 0; role < locations.size(); ++role)
		locations[role] = { uint32_t(role + 10), uint32_t(role + 20) };
	locations[4] = { NativePBR::kGpuMissingTexture, 0 };

	const auto record = NativePBR::PackGpuMaterialRecord(recipe, locations);
	CHECK((record.textures[0] == std::array<uint32_t, 4>{ 10, 20, 11, 21 }));
	CHECK((record.textures[1] == std::array<uint32_t, 4>{ 12, 22, 13, 23 }));
	CHECK((record.textures[2] == std::array<uint32_t, 4>{ NativePBR::kGpuMissingTexture, 0, 15, 25 }));
	CHECK((record.textures[3] == std::array<uint32_t, 4>{ 16, 26, NativePBR::kGpuMissingTexture, 0 }));
	CHECK(record.controls[0] == 0x6fu);
	CHECK(record.controls[1] == 1u);
	CHECK(record.properties[3] == recipe.issues);
	CHECK(record.base[1] == 0.67f);
	CHECK(record.base[2] == 1.7f);
	CHECK(record.projected[2] == 0.4f);
	CHECK((record.identity == std::array<uint32_t, 4>{ 0x55667788u, 0x11223344u, 0xddeeff00u, 0x99aabbccu }));
}

TEST_CASE("NativePBR GPU records distinguish unresident sources from missing material identities", "[NativePBR][gpu-material]")
{
	NativePBR::Recipe recipe;
	recipe.identity = 25;
	recipe.revision = 3;
	NativePBR::GpuTextureLocations locations{};
	for (auto& location : locations)
		location = { NativePBR::kGpuMissingTexture, 0 };
	const auto record = NativePBR::PackGpuMaterialRecord(recipe, locations);
	CHECK(record.controls[0] == 0u);
	CHECK(record.identity[0] == 25u);
	CHECK(record.identity[2] == 3u);
	CHECK(record.textures[3][2] == NativePBR::kGpuMissingTexture);
}

TEST_CASE("NativePBR GPU draw bindings preserve full material identity and revision", "[NativePBR][gpu-material]")
{
	const auto binding = NativePBR::PackGpuDrawBinding(0x1122334455667788ull,
		0x99aabbccddeeff00ull, 17u, NativePBR::kGpuDrawProjected | NativePBR::kGpuDrawUsesVertexColor);
	CHECK((binding.identity == std::array<uint32_t, 2>{ 0x55667788u, 0x11223344u }));
	CHECK((binding.revision == std::array<uint32_t, 2>{ 0xddeeff00u, 0x99aabbccu }));
	CHECK(binding.materialIndex == 17u);
	CHECK(binding.flags == 3u);
}

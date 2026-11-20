#include "Features/DX12Resources/PgReferenceRegistry.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace
{
	struct PgIndexFixture
	{
		std::filesystem::path root = std::filesystem::temp_directory_path() /
			("pg-reference-unit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		nlohmann::json value;

		PgIndexFixture()
		{
			std::filesystem::create_directories(root);
			value = {
				{ "kind", "dx12-pg-references" }, { "schemaVersion", 2 },
				{ "discoveryScope", "pg-patcher-outputs-only" },
				{ "generatedBy", "explicit-confirmed-user-action" },
				{ "generatedUtc", "2026-11-20T06:00:00Z" },
				{ "pgDiffSha256", std::string(64, 'a') },
				{ "deploymentSha256", std::string(64, 'b') },
				{ "pluginsSha256", std::string(64, 'c') },
				{ "meshCount", 1 }, { "shapeCount", 2 },
				{ "materialCount", 2 }, { "textureCount", 2 }, { "unresolved", 0 },
				{ "textures", { "textures/pbr/landscape/snow01.dds", "textures/pbr/landscape/glacierslab.dds" } },
				{ "materials", nlohmann::json::array({
					{ { "id", std::string(64, 'd') }, { "classification", "snow" },
					  { "shaderType", "default" }, { "projection", "projected-shader-flag; parameters-unresolved" },
					  { "shaderFlags1", 0 }, { "shaderFlags2", 0 }, { "alpha", 1.0 },
					  { "textureSlots", { 0, -1, -1, -1, -1, -1, -1, -1, -1 } } },
					{ { "id", std::string(64, 'e') }, { "classification", "ice" },
					  { "shaderType", "default" }, { "projection", "none" },
					  { "shaderFlags1", 0 }, { "shaderFlags2", 0 }, { "alpha", 1.0 },
					  { "textureSlots", { 1, -1, -1, -1, -1, -1, -1, -1, -1 } } }
				}) },
				{ "meshes", nlohmann::json::array({ {
					{ "path", "meshes\\landscape\\snowice.nif" },
					{ "sha256", std::string(64, 'f') }, { "sourcePackage", nullptr },
					{ "shapes", nlohmann::json::array({
						{ { "blockIndex", 3 }, { "name", "Snow" }, { "materialIndex", 0 },
						  { "snowCoverage", "projected-eligible" } },
						{ { "blockIndex", 5 }, { "name", "Ice" }, { "materialIndex", 1 },
						  { "snowCoverage", "none" } }
					}) }
				} }) }
			};
			Save();
		}
		~PgIndexFixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
		void Save() const { std::ofstream(root / "index-v2.json") << value.dump(); }
	};
}

TEST_CASE("PG reference index resolves stable mesh and block bindings without discovery", "[dx12_resources][pg_index]")
{
	PgIndexFixture fixture;
	const auto registry = DX12ResourceService::PgReferenceRegistry::Open(fixture.root);
	REQUIRE(registry->shapeCount == 2);
	REQUIRE(registry->textures.size() == 2);
	const auto snow = registry->Find("Meshes/Landscape/SnowIce.nif", 3);
	REQUIRE(snow);
	REQUIRE(snow->material->classification == "snow");
	REQUIRE(snow->shape->snowCoverage == "projected-eligible");
	REQUIRE(registry->textures.at(snow->material->textureSlots[0]) == "textures/pbr/landscape/snow01.dds");
	const auto ice = registry->Find("landscape/snowice.nif", 5);
	REQUIRE(ice);
	REQUIRE(ice->material->classification == "ice");
	REQUIRE(ice->shape->snowCoverage == "none");
	REQUIRE_FALSE(registry->Find("landscape/snowice.nif", 9));
	REQUIRE_FALSE(registry->Find("landscape/unindexed.nif", 3));
}

TEST_CASE("PG reference index rejects mismatched or out-of-range identities", "[dx12_resources][pg_index]")
{
	PgIndexFixture fixture;
	fixture.value["materials"][0]["textureSlots"][0] = 999;
	fixture.Save();
	REQUIRE_THROWS(DX12ResourceService::PgReferenceRegistry::Open(fixture.root));
	fixture.value["materials"][0]["textureSlots"][0] = 0;
	fixture.value["discoveryScope"] = "global-nif-scan";
	fixture.Save();
	REQUIRE_THROWS(DX12ResourceService::PgReferenceRegistry::Open(fixture.root));
}

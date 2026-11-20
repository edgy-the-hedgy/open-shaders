#include "Features/DX12Resources/GlobalSnowAtlas.h"
#include "Utils/FileDigest.h"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>

namespace
{
	struct SnowAtlasFixture
	{
		std::filesystem::path root;
		std::string digest;
		SnowAtlasFixture()
		{
			root = std::filesystem::temp_directory_path() /
				("snow-atlas-unit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(root / "textures");
			std::array<uint8_t, 128> bytes{};
			bytes[0] = 'D'; bytes[1] = 'D'; bytes[2] = 'S'; bytes[3] = ' ';
			bytes[4] = 124; bytes[12] = 4; bytes[16] = 4; bytes[28] = 1; bytes[76] = 32;
			const auto hash = Util::FileDigest::Sha256BytesHex(bytes);
			REQUIRE(hash);
			digest = *hash;
			std::ranges::transform(digest, digest.begin(), [](unsigned char c) { return char(std::tolower(c)); });
			std::ofstream output(root / "textures" / (digest + ".dds"), std::ios::binary);
			output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			output.close();
			WriteManifest();
		}
		~SnowAtlasFixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
		void WriteManifest()
		{
			const nlohmann::json value = {
				{ "kind", "dx12-global-snow-atlas" }, { "schemaVersion", 1 },
				{ "scope", "global" }, { "cellIndependent", true },
				{ "configurationFingerprint", std::string(64, 'a') },
				{ "contentFingerprint", std::string(64, 'b') },
				{ "unresolvedResources", 0 }, { "uniqueDdsBlobs", 1 },
				{ "materials", nlohmann::json::array({ {
					{ "id", std::string(64, 'c') }, { "model", "meshes\\sample.nif" },
					{ "shapeBlock", 0 }, { "textures", { "textures\\landscape\\snow01.dds" } }
				} }) },
				{ "textures", nlohmann::json::array({ {
					{ "path", "textures\\landscape\\snow01.dds" }, { "status", "cached" },
					{ "origin", "archive:Skyrim - Textures.bsa" },
					{ "roles", { "base-color:srgb" } },
					{ "sha256", digest }, { "file", "textures/" + digest + ".dds" },
					{ "width", 4 }, { "height", 4 }, { "mips", 1 }
				} }) }
			};
			std::ofstream(root / "manifest.json") << value.dump();
		}
	};
}

TEST_CASE("Global snow inventory is independent of CELL cache selection", "[dx12_resources][snow_atlas]")
{
	SnowAtlasFixture fixture;
	const auto first = DX12ResourceService::GlobalSnowAtlas::Open(fixture.root);
	REQUIRE(first->textures.size() == 1);
	REQUIRE(first->materials.size() == 1);
	const auto resolved = first->Resolve("Textures/Landscape/Snow01.DDS");
	REQUIRE(resolved);
	REQUIRE(resolved->status == "cached");
	REQUIRE_FALSE(resolved->resident);
	REQUIRE(resolved->page == UINT32_MAX);
	std::filesystem::create_directories(fixture.root / "cells" / "Unrelated.esp");
	std::ofstream(fixture.root / "cells" / "Unrelated.esp" / "cell-layout.json") << "new cell";
	const auto second = DX12ResourceService::GlobalSnowAtlas::Open(fixture.root);
	REQUIRE(second->contentFingerprint == first->contentFingerprint);
	REQUIRE(second->materials.size() == first->materials.size());
}

TEST_CASE("Global snow catalog rejects tampered cached DDS content", "[dx12_resources][snow_atlas]")
{
	SnowAtlasFixture fixture;
	std::fstream output(fixture.root / "textures" / (fixture.digest + ".dds"), std::ios::binary | std::ios::in | std::ios::out);
	output.seekp(20);
	output.put('\x01');
	output.close();
	REQUIRE_NOTHROW(DX12ResourceService::GlobalSnowAtlas::Open(fixture.root, false));
	REQUIRE_THROWS(DX12ResourceService::GlobalSnowAtlas::Open(fixture.root, true));
}

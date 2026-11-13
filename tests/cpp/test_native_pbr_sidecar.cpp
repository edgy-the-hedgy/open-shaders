#include <catch2/catch_test_macros.hpp>
#include "Features/NativePBR/MaterialSidecarContract.h"

#include <limits>

TEST_CASE("NativePBR sidecar encodes a full 64-bit registry identity and frame-local draw ordinal", "[NativePBR]")
{
	constexpr uint64_t material = 0xFEDCBA9876543210ull;
	const std::array<NativePBR::SidecarDraw, 3> draws{ { { 1, 0x0000000076543210ull, 1 }, { 2, material, 20 }, { 3, 9, 1 } } };
	REQUIRE(NativePBR::ResolveSidecarMaterial(2, draws) == material);
	REQUIRE(NativePBR::ResolveSidecarMaterial(1, draws) == 0x76543210u);
	REQUIRE(NativePBR::ResolveSidecarMaterial(0, draws) == 0);
	REQUIRE(NativePBR::ResolveSidecarMaterial(4, draws) == 0);
	const std::array<NativePBR::SidecarDraw, 1> mismatched{ { { 31, material, 1 } } };
	REQUIRE(NativePBR::ResolveSidecarMaterial(1, mismatched) == 0);
}

TEST_CASE("NativePBR coverage rejects different final depth and invalid depth", "[NativePBR]")
{
	REQUIRE(NativePBR::SidecarDepthMatches(0.25f, 0.25f, false));
	REQUIRE_FALSE(NativePBR::SidecarDepthMatches(0.25f, 0.251f, false));
	REQUIRE_FALSE(NativePBR::SidecarDepthMatches(0.25f, 0.251f, true));
	REQUIRE_FALSE(NativePBR::SidecarDepthMatches(-1.0f, 0.0f, true));
	REQUIRE_FALSE(NativePBR::SidecarDepthMatches(0.0f, 2.0f, true));
	REQUIRE_FALSE(NativePBR::SidecarDepthMatches(std::numeric_limits<float>::quiet_NaN(), 0.5f, false));
	REQUIRE_FALSE(NativePBR::SidecarDepthMatches(0.5f, std::numeric_limits<float>::infinity(), false));
}

TEST_CASE("NativePBR sidecar retains six independent outputs without using deferred MRTs", "[NativePBR]")
{
	REQUIRE(NativePBR::kSidecarBytesPerPixel.size() == 6u);
	uint32_t bytes = 0;
	for (auto size : NativePBR::kSidecarBytesPerPixel)
		bytes += size;
	REQUIRE(bytes == 60u);
	REQUIRE(static_cast<size_t>(NativePBR::SidecarTarget::Identity) == 0);
	REQUIRE(static_cast<size_t>(NativePBR::SidecarTarget::Basis2) == 5);
}

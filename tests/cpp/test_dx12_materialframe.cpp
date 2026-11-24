#include "Features/DX12/DX12MaterialFrameContract.h"

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

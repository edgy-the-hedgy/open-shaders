#include "Features/DX12/RuntimeResidencyPolicy.h"
#include "Features/DX12/SpatialGrid.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

TEST_CASE("Runtime DX12 residency rejects over-budget shapes", "[dx12]")
{
	using namespace DX12;
	REQUIRE(RuntimeShapeFits(0, 0, 12));
	REQUIRE(RuntimeShapeFits(kMaximumRuntimeShapes - 1,
		kMaximumRuntimeBytes - kMaximumSingleRuntimeShapeBytes, kMaximumSingleRuntimeShapeBytes));
	REQUIRE_FALSE(RuntimeShapeFits(kMaximumRuntimeShapes, 0, 12));
	REQUIRE_FALSE(RuntimeShapeFits(0, 0, 0));
	REQUIRE_FALSE(RuntimeShapeFits(0, 0, kMaximumSingleRuntimeShapeBytes + 1));
	REQUIRE_FALSE(RuntimeShapeFits(0, kMaximumRuntimeBytes, 1));
	REQUIRE_FALSE(RuntimeShapeFits(0, std::numeric_limits<uint64_t>::max(), 1));
}

TEST_CASE("Runtime DX12 publication requires the same scope and epoch", "[dx12]")
{
	using DX12::RuntimeSceneMatches;
	REQUIRE(RuntimeSceneMatches(12, 4, 12, 4));
	REQUIRE_FALSE(RuntimeSceneMatches(12, 4, 12, 5));
	REQUIRE_FALSE(RuntimeSceneMatches(12, 4, 13, 4));
	REQUIRE_FALSE(RuntimeSceneMatches(0, 4, 0, 4));
}

TEST_CASE("Runtime DX12 neighborhoods contain nine scoped tiles", "[dx12]")
{
	const auto grid = DX12::Neighborhood(42, 4096, -1.0, 4096.0);
	REQUIRE(grid.has_value());
	REQUIRE(grid->size() == 9);
	REQUIRE(grid->front().scope == 42);
	REQUIRE(grid->front().x == -1);
	REQUIRE(grid->front().y == 1);
	for (const auto& key : *grid)
		REQUIRE(key.scope == 42);
	REQUIRE_FALSE(DX12::Neighborhood(0, 4096, 0, 0).has_value());
	REQUIRE_FALSE(DX12::Neighborhood(42, 1024, 0, 0).has_value());
}

TEST_CASE("Runtime geometry expiry tolerates frame rollover", "[dx12]")
{
	using namespace DX12;
	REQUIRE_FALSE(RuntimeObservationExpired(700, 100));
	REQUIRE(RuntimeObservationExpired(701, 100));
	REQUIRE_FALSE(RuntimeObservationExpired(3, UINT32_MAX - 3));
	REQUIRE(RuntimeObservationExpired(1000, UINT32_MAX - 3));
}

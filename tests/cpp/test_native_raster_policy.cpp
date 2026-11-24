#include "Materials/NativeRasterPolicy.h"

#include <catch2/catch_test_macros.hpp>

using namespace NativeRasterPolicy;

TEST_CASE("Native raster routing rejects nondefault and unsupported draws", "[dx12][materials]")
{
	constexpr uint32_t projected = 1u << 15;
	REQUIRE(RoutePass(0u, true, projected) == kRouteBit);
	REQUIRE(RoutePass(kRouteBit, false, projected) == 0u);
	REQUIRE(RoutePass(projected, true, projected) == projected);
	REQUIRE(RoutePass(8u << 24, true, projected) == (8u << 24));
}

TEST_CASE("Native raster marker becomes a distinct deferred permutation", "[dx12][materials]")
{
	constexpr uint32_t vc = 1u;
	REQUIRE(NormalizePixel(vc | kRouteBit, true, true) == (vc | kInputBit));
	REQUIRE(NormalizePixel(vc | kRouteBit, true, false) == vc);
	REQUIRE(NormalizePixel(vc | kRouteBit, false, true) == vc);
	REQUIRE(NormalizePixel(vc, true, true) == vc);
}

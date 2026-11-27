#include "Features/DX12/DX12LightMath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

TEST_CASE("DX12 light conversion preserves linear intensity", "[dx12][lighting]")
{
	using DX12Native::LightMath::ToLinear;
	REQUIRE(ToLinear(0.5f, false) == Approx(0.214041f));
	REQUIRE(ToLinear(0.5f, true) == Approx(0.5f));
	REQUIRE(ToLinear(4.0f, true) == Approx(4.0f));
	REQUIRE(ToLinear(-0.5f, false) == Approx(0.0f));
	REQUIRE(ToLinear(0.02f, false) == Approx(0.02f / 12.92f));
}

TEST_CASE("DX12 prepared light matching has bounded world tolerance", "[dx12][lighting]")
{
	using DX12Native::LightMath::MatchTolerance;
	REQUIRE(MatchTolerance(16.0f) == Approx(4.0f));
	REQUIRE(MatchTolerance(1000.0f) == Approx(10.0f));
	REQUIRE(MatchTolerance(60000.0f) == Approx(32.0f));
}

TEST_CASE("Full Native point emitter matching rejects conflicting placed references", "[dx12][lighting]")
{
	using DX12Native::LightMath::SameEmitterCandidate;
	REQUIRE(SameEmitterCandidate(11, 11, 4.0f, 1000.0f));
	REQUIRE(SameEmitterCandidate(0, 11, 4.0f, 1000.0f));
	REQUIRE_FALSE(SameEmitterCandidate(11, 12, 0.0f, 1000.0f));
	REQUIRE_FALSE(SameEmitterCandidate(11, 11, 121.0f, 1000.0f));
	REQUIRE_FALSE(SameEmitterCandidate(0, 0, 0.0f, -1.0f));
}

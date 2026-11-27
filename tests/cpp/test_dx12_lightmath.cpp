#include "Features/DX12/DX12LightMath.h"
#include "Features/NativeLLF/NativeLightSpatialPolicy.h"

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

TEST_CASE("Native spotlight footprint matches the legacy cone falloff", "[dx12][lighting]")
{
	using DX12Native::LightMath::SpotFootprint;
	REQUIRE(SpotFootprint(0, 0, 1) == 1.0f);
	REQUIRE(SpotFootprint(1, 0, 2) == 0.75f);
	REQUIRE(SpotFootprint(2, 0, 2) == 0.0f);
	REQUIRE(SpotFootprint(3, 0, 2) == 0.0f);
	REQUIRE(SpotFootprint(0, 0, -1) == 0.0f);
}

TEST_CASE("Native spatial grouping retains prepared authority and evaluates live radiance", "[dx12][lighting]")
{
	using namespace NativeLLF::SpatialPolicy;
	Publication publication;
	publication.signature.scope = 1;
	publication.signature.revision = 2;
	publication.signature.prepared = { { { 0, 0, 0 }, 1, 0 } };
	publication.signature.runtime = { { { 100, 0, 0 }, 2, 10 }, { { 500, 0, 0 }, 3, 20 }, { { 600, 0, 0 }, 3, 30 }, { { 510, 0, 0 }, 4, 40 } };
	BuildGrouping(publication);
	REQUIRE(publication.grouping.preparedRuntime[0].size() == 1);
	REQUIRE(publication.grouping.runtimeGroups.size() == 2);
	struct Light { Position position, radiance; float radius, sourceRadius; };
	const std::vector<Light> prepared{ { {0,0,0}, {1,0,0}, 50, 0 } };
	std::vector<Light> runtime{ { {100,0,0}, {2,0,0}, 50, 0 }, { {500,0,0}, {1,0,0}, 60, 0 }, { {600,0,0}, {3,0,0}, 80, 0 }, { {510,0,0}, {5,0,0}, 40, 0 } };
	const auto merged = ApplyGrouping<Light>(publication.grouping, prepared, runtime);
	REQUIRE(merged.size() == 3);
	REQUIRE(merged[0].radiance.x == Approx(3.0f));
	REQUIRE(merged[1].radiance.x == Approx(4.0f));
	REQUIRE(merged[1].position.x == Approx(575.0f));
	REQUIRE(merged[1].radius == Approx(80.0f));
	runtime[0].radiance.x = 8;
	REQUIRE(ApplyGrouping<Light>(publication.grouping, prepared, runtime)[0].radiance.x == Approx(9.0f));
	auto changed = publication.signature;
	changed.runtime.pop_back();
	REQUIRE_FALSE(SameSignature(publication.signature, changed));
	changed = publication.signature;
	changed.scope++;
	REQUIRE_FALSE(SameSignature(publication.signature, changed));
	changed = publication.signature;
	changed.settings.additive = false;
	REQUIRE_FALSE(SameSignature(publication.signature, changed));
	publication.signature.settings.additive = false;
	BuildGrouping(publication);
	REQUIRE(publication.grouping.preparedRuntime[0].empty());
}

#include <catch2/catch_test_macros.hpp>

#include "Features/NativePBR/NativeSnowObservations.h"

TEST_CASE("Native snow separates NIF and original shader-state authority", "[nativepbr][snow]")
{
	using namespace NativePBR;
	SnowSourceState source;
	CHECK(ClassifySnow(source) == 0);
	source.renderPassDescriptor = 0;
	CHECK(ClassifySnow(source) == 0);
	source.renderPassDescriptor = kSnowShaderFlag;
	CHECK(ClassifySnow(source) == SnowOrdinary);
	source.renderPassDescriptor = kBaseObjectSnowShaderFlag;
	CHECK(ClassifySnow(source) == SnowBaseObject);
	source.renderPassDescriptor = kSnowShaderFlag | kBaseObjectSnowShaderFlag;
	CHECK(ClassifySnow(source) == (SnowOrdinary | SnowBaseObject));
	source.renderPassDescriptor = kProjectedUvShaderFlag;
	CHECK(ClassifySnow(source) == SnowProjected);
	source.nifFlags = kSnowNifFlag;
	CHECK(ClassifySnow(source) == (SnowOrdinary | SnowProjected));
	source.nifFlags = kLandscapeNifFlag;
	CHECK(ClassifySnow(source) == (SnowLandscape | SnowProjected));
	CHECK(SnowPassesAgree(kSnowShaderFlag, kSnowShaderFlag | (1u << 9)));
	CHECK_FALSE(SnowPassesAgree(kSnowShaderFlag, kBaseObjectSnowShaderFlag));
	SnowSourceState observed;
	observed.runtimeNifFlags = kSnowNifFlag;
	const auto consistent = BindSnowSource(kSnowNifFlag, observed);
	CHECK_FALSE(consistent.conflictingPasses);
	CHECK(consistent.nifFlags == kSnowNifFlag);
	const auto stale = BindSnowSource(kProjectedUvNifFlag, observed);
	CHECK(stale.conflictingPasses);
	CHECK(stale.nifFlags == kProjectedUvNifFlag);
}

TEST_CASE("Native snow observations retain conflicts and reset scene generations", "[nativepbr][snow]")
{
	using namespace NativePBR;
	NativeSnowObservations observed;
	REQUIRE(observed.Observe("123:Skyrim.esm", "SnowCliff", kSnowShaderFlag, kSnowNifFlag));
	auto first = observed.ExportIfChanged(0);
	REQUIRE(first);
	CHECK(first->entries.size() == 1);
	CHECK_FALSE(first->entries.begin()->second.conflictingPasses);
	CHECK_FALSE(observed.Observe("123:skyrim.ESM", "snowcliff", kSnowShaderFlag | (1u << 9), kSnowNifFlag));
	CHECK_FALSE(observed.ExportIfChanged(first->revision));
	REQUIRE(observed.Observe("123:Skyrim.esm", "SnowCliff", kProjectedUvShaderFlag, kSnowNifFlag));
	auto second = observed.ExportIfChanged(first->revision);
	REQUIRE(second);
	CHECK(second->entries.begin()->second.conflictingPasses);
	CHECK_FALSE(first->entries.begin()->second.conflictingPasses);
	observed.Reset();
	auto after = observed.ExportIfChanged(second->revision);
	REQUIRE(after);
	CHECK(after->entries.empty());
	CHECK(after->epoch == second->epoch + 1);
}

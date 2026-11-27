#include "Materials/NativeRasterPolicy.h"
#include "Features/DX12/DX12ControlMode.h"

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

TEST_CASE("Disabled native raster strips stale route and input bits", "[dx12][materials]")
{
	constexpr uint32_t preserved = (1u << 20) | (8u << 24) | (1u << 4);
	constexpr uint32_t stale = preserved | kRouteBit | kInputBit;
	REQUIRE(StripRoute(stale) == preserved);
	REQUIRE(NormalizePixel(stale, false, true) == preserved);
	REQUIRE(NormalizePixel(stale, false, false) == preserved);
	REQUIRE(NormalizePixel(preserved | kInputBit, true, true) == preserved);
}

TEST_CASE("Full Native input ABI is independent of fallback and runtime producer state", "[dx12][shaders]")
{
	using namespace DX12;
	for (auto kind : { InputShaderKind::Lighting, InputShaderKind::Effect, InputShaderKind::Sky, InputShaderKind::DistantTree }) {
		REQUIRE(HasFullNativeInput(FeatureBackend::FullNative, kind));
		REQUIRE_FALSE(HasFullNativeInput(FeatureBackend::Baseline, kind));
	}
	REQUIRE_FALSE(HasFullNativeInput(FeatureBackend::FullNative, InputShaderKind::Other));
	for (auto mode : { ControlMode::ShadowOffload, ControlMode::RasterComparison, ControlMode::FullRT })
		REQUIRE(SelectFeatureBackend(true, mode, false) == FeatureBackend::Baseline);
	REQUIRE(SelectFeatureBackend(false, ControlMode::FullNative, false) == FeatureBackend::Baseline);
	REQUIRE(SelectFeatureBackend(true, ControlMode::FullNative, true) == FeatureBackend::Baseline);
}

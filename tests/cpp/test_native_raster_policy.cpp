#include "Materials/NativeRasterPolicy.h"
#include "Features/NativePBR/NativeInputOnlyPolicy.h"
#include "Features/NativeLLF/NativeLightMetadataPolicy.h"
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
	for (auto kind : { InputShaderKind::Lighting, InputShaderKind::Effect, InputShaderKind::Sky, InputShaderKind::DistantTree, InputShaderKind::Water }) {
		REQUIRE(HasFullNativeInput(FeatureBackend::FullNative, kind));
		REQUIRE_FALSE(HasFullNativeInput(FeatureBackend::Baseline, kind));
	}
	REQUIRE_FALSE(HasFullNativeInput(FeatureBackend::FullNative, InputShaderKind::Other));
	for (auto mode : { ControlMode::ShadowOffload, ControlMode::RasterComparison, ControlMode::FullRT })
		REQUIRE(SelectFeatureBackend(true, mode, false) == FeatureBackend::Baseline);
	REQUIRE(SelectFeatureBackend(false, ControlMode::FullNative, false) == FeatureBackend::Baseline);
	REQUIRE(SelectFeatureBackend(true, ControlMode::FullNative, true) == FeatureBackend::Baseline);
}

TEST_CASE("Water input permutations are exclusive to an active Full Native session", "[dx12][shaders]")
{
	using namespace DX12;
	for (auto mode : { ControlMode::ShadowOffload, ControlMode::RasterComparison, ControlMode::FullRT, ControlMode::FullNative }) {
		for (bool active : { false, true }) {
			for (bool vr : { false, true }) {
				const auto backend = SelectFeatureBackend(active, mode, vr);
				REQUIRE(HasFullNativeInput(backend, InputShaderKind::Water) == (active && !vr && mode == ControlMode::FullNative));
			}
		}
	}
}

TEST_CASE("Full Native enforces exclusive counterpart authority", "[dx12][features]")
{
	using namespace DX12;
	REQUIRE(SelectExecutionRole(FeatureBackend::FullNative, CounterpartKind::LegacyMaterial) == ExecutionRole::Dormant);
	REQUIRE(SelectExecutionRole(FeatureBackend::FullNative, CounterpartKind::LegacyLighting) == ExecutionRole::Dormant);
	REQUIRE(SelectExecutionRole(FeatureBackend::FullNative, CounterpartKind::NativeProducer) == ExecutionRole::NativeAuthority);
	REQUIRE(SelectExecutionRole(FeatureBackend::Baseline, CounterpartKind::NativeProducer) == ExecutionRole::Dormant);
	REQUIRE(SelectExecutionRole(FeatureBackend::Baseline, CounterpartKind::LegacyLighting) == ExecutionRole::Baseline);
	REQUIRE(SelectExecutionRole(FeatureBackend::Baseline, CounterpartKind::LegacyMaterial) == ExecutionRole::Baseline);
	REQUIRE(SelectExecutionRole(FeatureBackend::FullNative, CounterpartKind::Shared) == ExecutionRole::Baseline);
	REQUIRE(SelectExecutionRole(FeatureBackend::Baseline, CounterpartKind::Shared) == ExecutionRole::Baseline);
}

TEST_CASE("Native authored light metadata sanitizes inputs without legacy state", "[dx12][lights]")
{
	using namespace NativeLLF::Policy;
	const auto normal = MakeAuthoredLight(42, true, false, 0.05f, 2.0f);
	REQUIRE(normal.formID == 42);
	REQUIRE(normal.inverseSquare);
	REQUIRE_FALSE(normal.spot);
	REQUIRE_FALSE(normal.linear);
	REQUIRE(MakeAuthoredLight(42, false, false, 0.5f, 1.0f, true).linear);
	REQUIRE(normal.cutoff == 0.05f);
	REQUIRE(normal.size == 2.0f);
	const auto spot = MakeAuthoredLight(7, false, true, -1.0f, 90.0f);
	REQUIRE(spot.spot);
	REQUIRE(spot.cutoff == 0.01f);
	REQUIRE(spot.size == std::numbers::sqrt2_v<float>);
	REQUIRE_FALSE(Expired(130, 10));
	REQUIRE(Expired(131, 10));
	REQUIRE_FALSE(Expired(1, UINT32_MAX));
}

TEST_CASE("Input-only raster capture rejects partial draws and invalid integer identities", "[dx12][native-pbr]")
{
	using namespace NativePBR::InputOnly;
	REQUIRE(WholeTriangleList(3,1));
	REQUIRE(WholeTriangleList(196605,65535));
	REQUIRE_FALSE(WholeTriangleList(3,2));
	REQUIRE_FALSE(WholeTriangleList(0,0));
	REQUIRE(WholeTriangleList(196608,65536));
	REQUIRE(WholeTriangleList(3000000u,1000000u));
	REQUIRE(ValidIdentity(0,65535));
	REQUIRE(ValidIdentity(65535,1000000u));
	REQUIRE_FALSE(ValidIdentity(UINT32_MAX,1));
	REQUIRE_FALSE(ValidIdentity(1,0));
	constexpr auto covered = PackCoverage(true, kAlphaMax);
	REQUIRE((covered & (kValid | kCovered | kFrontFace)) == (kValid | kCovered | kFrontFace));
	REQUIRE((covered >> kAlphaShift) == kAlphaMax);
	constexpr DrawMetadata opaque{};
	REQUIRE_FALSE(opaque.AlphaTest());
	DrawMetadata cutout{};
	cutout.pixelDescriptor = kAlphaTestFlag;
	REQUIRE(cutout.AlphaTest());
}

#include "Features/NativePBR/MaterialRouting.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("NativePBR compilation routing is independent of DX12 initialization", "[native_pbr]")
{
	NativePBR::RoutingInputs input;
	input.nativeSelected = true;
	input.featureLoaded = true;
	input.scope = NativePBR::MaterialScope::OpaqueStaticMesh;
	const auto unavailable = NativePBR::ResolveRouting(input);
	REQUIRE(unavailable.shaderDomain == NativePBR::ShaderDomain::NativePBRInput);
	REQUIRE(unavailable.owner == NativePBR::RenderingOwner::NativeUnavailable);

	input.sceneOwnsNative = true;
	const auto active = NativePBR::ResolveRouting(input);
	REQUIRE(active.shaderDomain == unavailable.shaderDomain);
	REQUIRE(active.owner == NativePBR::RenderingOwner::Native);

	input.materialInputsAvailable = false;
	input.publishedNativeMaterialGeneration = 0;
	REQUIRE(NativePBR::ResolveRouting(input).owner == NativePBR::RenderingOwner::Native);
	input.materialInputsAvailable = true;
	input.publishedNativeMaterialGeneration = 7;
	REQUIRE(NativePBR::ResolveRouting(input).publishedNativeMaterialGeneration == 7);

	input.legacyComparisonRequested = true;
	const auto comparison = NativePBR::ResolveRouting(input);
	REQUIRE(comparison.owner == NativePBR::RenderingOwner::Legacy);
	REQUIRE(comparison.shaderDomain == NativePBR::ShaderDomain::LegacyTruePBR);
	REQUIRE(comparison.legacyComparisonRequested);

	input.legacyComparisonRequested = false;
	input.scope = NativePBR::MaterialScope::Unmigrated;
	REQUIRE(NativePBR::ResolveRouting(input).owner == NativePBR::RenderingOwner::Legacy);
	REQUIRE(NativePBR::ResolveRouting(input).shaderDomain == NativePBR::ShaderDomain::LegacyTruePBR);
	input.scope = NativePBR::MaterialScope::OpaqueStaticMesh;
	input.nativeSelected = false;
	REQUIRE(NativePBR::ResolveRouting(input).owner == NativePBR::RenderingOwner::Legacy);
}

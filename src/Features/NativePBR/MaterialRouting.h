#pragma once

#include <cstdint>

namespace NativePBR
{
	enum class Backend : uint8_t
	{
		LegacyDX11,
		NativeDX12
	};

	enum class MaterialScope : uint8_t
	{
		OpaqueStaticMesh,
		ProjectedStaticMesh,
		Unmigrated
	};

	enum class ShaderDomain : uint8_t
	{
		LegacyTruePBR,
		NativePBRInput,
		Unavailable
	};

	enum class RenderingOwner : uint8_t
	{
		Legacy,
		Native,
		NativeUnavailable
	};

	/** @brief Describes independently observed inputs to NativePBR routing. */
	struct RoutingInputs
	{
		Backend backend = Backend::LegacyDX11;
		bool nativeFeatureEnabled = false;
		bool featureLoaded = false;
		bool sceneOwnsNative = false;
		bool legacyComparisonRequested = false;
		bool materialInputsAvailable = false;
		uint64_t publishedNativeMaterialGeneration = 0;
		MaterialScope scope = MaterialScope::Unmigrated;
	};

	/** @brief Separates the compilation policy from current rendering readiness. */
	struct RoutingState
	{
		Backend backend = Backend::LegacyDX11;
		ShaderDomain shaderDomain = ShaderDomain::LegacyTruePBR;
		RenderingOwner owner = RenderingOwner::Legacy;
		bool materialInputsAvailable = false;
		uint64_t publishedNativeMaterialGeneration = 0;
		bool legacyComparisonRequested = false;
	};

	/** @brief Resolves explicit material ownership without silently substituting another material source. */
	constexpr RoutingState ResolveRouting(const RoutingInputs& a_inputs)
	{
		RoutingState result;
		result.backend = a_inputs.backend;
		result.materialInputsAvailable = a_inputs.materialInputsAvailable;
		result.publishedNativeMaterialGeneration = a_inputs.publishedNativeMaterialGeneration;
		result.legacyComparisonRequested = a_inputs.legacyComparisonRequested;
		if (a_inputs.backend == Backend::LegacyDX11)
			return result;

		result.owner = RenderingOwner::NativeUnavailable;
		result.shaderDomain = ShaderDomain::Unavailable;
		if (a_inputs.scope == MaterialScope::Unmigrated)
			return result;

		result.shaderDomain = ShaderDomain::NativePBRInput;
		result.owner = a_inputs.nativeFeatureEnabled && a_inputs.featureLoaded && a_inputs.sceneOwnsNative ?
			RenderingOwner::Native : RenderingOwner::NativeUnavailable;
		return result;
	}
}

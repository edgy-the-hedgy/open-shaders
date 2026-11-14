#pragma once

#include "Features/NativePBR/MaterialRouting.h"
#include <string>
#include <string_view>

namespace NativePBR
{
	inline constexpr std::string_view kInputDomainIdentity = "NATIVE_PBR_INPUT:v1";
	inline constexpr std::string_view kInputDomainRelativePrefix = "NATIVE_PBR_INPUT/v1/";
	inline constexpr std::wstring_view kInputDomainDirectory = L"NATIVE_PBR_INPUT/v1";

	/** @brief Restores the original shader-source-relative layout for cache pruning. */
	constexpr std::string_view SourceRelativePath(std::string_view a_cachePath)
	{
		if (a_cachePath.starts_with(kInputDomainRelativePrefix))
			a_cachePath.remove_prefix(kInputDomainRelativePrefix.size());
		return a_cachePath;
	}

	/** @brief Keeps the NativePBR FXC cache isolated from fully shaded TruePBR bytecode. */
	inline std::string CacheKeyForDomain(std::string a_baseKey, ShaderDomain a_domain)
	{
		if (a_domain == ShaderDomain::NativePBRInput) {
			a_baseKey += ':';
			a_baseKey += kInputDomainIdentity;
		}
		return a_baseKey;
	}
}

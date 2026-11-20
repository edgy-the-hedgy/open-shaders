#pragma once

#include "NativePBRCompiler.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace NativePBR
{
	struct SnowTextureViewSource
	{
		const void* resourceView = nullptr;
		std::string_view texturePath;
	};

	/** @brief Accepts an engine DDS path only when its source view is the actual bound pixel-shader view. */
	inline std::optional<std::string> IdentifySnowTextureView(const void* a_boundView,
		std::span<const SnowTextureViewSource> a_sources)
	{
		if (!a_boundView)
			return std::nullopt;
		std::optional<std::string> identified;
		for (const auto& source : a_sources) {
			if (source.resourceView != a_boundView || source.texturePath.empty())
				continue;
			std::string path = Util::ToLowerAscii(std::string(source.texturePath));
			std::replace(path.begin(), path.end(), '\\', '/');
			if (path.starts_with("data/"))
				path.erase(0, 5);
			const auto canonical = CanonicalTexturePath(path);
			if (!canonical || !canonical->ends_with(".dds"))
				continue;
			if (identified && *identified != *canonical)
				return std::nullopt;
			identified = *canonical;
		}
		return identified;
	}
}

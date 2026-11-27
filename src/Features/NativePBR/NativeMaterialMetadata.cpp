#include "NativeMaterialMetadata.h"

#include "RE/B/BSLightingShaderProperty.h"
#include "RE/B/BSTextureSet.h"
#include "RE/B/BGSTextureSet.h"
#include "RE/N/NiIntegersExtraData.h"
#include "RE/N/NiFloatsExtraData.h"
#include "Utils/StringUtils.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace NativePBR
{
	namespace
	{
		constexpr auto kAuthoredFlags = "OS.NativePBR.OriginalFlags.v1";
		constexpr size_t kMaxTextureSources = 32768;
		struct TextureSource
		{
			std::string signature, editorId;
			bool ambiguous = false;
		};
		std::mutex textureMutex;
		std::unordered_map<const RE::BSTextureSet*, TextureSource> textureSources;

		std::string TextureSignature(RE::BSTextureSet* textures)
		{
			std::string key;
			for (uint32_t i = 0; i < 8; ++i) {
				const char* path = textures->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i));
				auto name = Util::ToLowerAscii(path ? path : "");
				if (name.size() > 260)
					return {};
				std::ranges::replace(name, '/', '\\');
				key += std::to_string(name.size()) + ":" + name;
			}
			return key;
		}

		template <size_t N>
		std::optional<std::array<float, N>> ReadFloats(const RE::BSLightingShaderProperty& property, const char* name)
		{
			const auto* data = netimmerse_cast<const RE::NiFloatsExtraData*>(property.GetExtraData(name));
			if (!data || data->size != N || !data->value)
				return {};
			std::array<float, N> values{};
			std::copy_n(data->value, N, values.begin());
			if (!std::ranges::all_of(values, [](float value) { return std::isfinite(value); }))
				return {};
			return values;
		}
	}

	void PreserveAuthoredFlags(RE::BSLightingShaderProperty& property)
	{
		if (property.HasExtraData(kAuthoredFlags))
			return;
		const uint64_t flags = static_cast<uint64_t>(property.flags.underlying());
		const std::vector<int32_t> words{
			std::bit_cast<int32_t>(uint32_t(flags)),
			std::bit_cast<int32_t>(uint32_t(flags >> 32))
		};
		RE::NiPointer<RE::NiIntegersExtraData> metadata{ RE::NiIntegersExtraData::Create(kAuthoredFlags, words) };
		if (metadata)
			property.AddExtraData(metadata.get());
	}

	std::optional<uint64_t> ReadAuthoredFlags(const RE::BSLightingShaderProperty& property)
	{
		const auto* data = netimmerse_cast<const RE::NiIntegersExtraData*>(property.GetExtraData(kAuthoredFlags));
		if (!data || data->size != 2 || !data->value)
			return {};
		const uint64_t flags = uint64_t(uint32_t(data->value[0])) | (uint64_t(uint32_t(data->value[1])) << 32);
		if (!(flags & static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kMenuScreen)))
			return {};
		return flags;
	}

	void RememberTextureSet(RE::BGSTextureSet* record, RE::BSTextureSet* converted)
	{
		if (!record || !converted)
			return;
		const char* id = record->GetFormEditorID();
		if (!id || !*id || std::string_view(id).size() > 256)
			return;
		auto signature = TextureSignature(converted);
		if (signature.empty())
			return;
		std::scoped_lock lock(textureMutex);
		if (textureSources.size() >= kMaxTextureSources && !textureSources.contains(converted))
			textureSources.clear();
		auto& entry = textureSources[converted];
		if (entry.signature != signature)
			entry = { std::move(signature), id, false };
		else if (entry.editorId != id)
			entry.ambiguous = true;
	}

	std::string ResolveTextureSet(RE::BSTextureSet* textures)
	{
		if (!textures)
			return {};
		if (const auto* record = skyrim_cast<RE::BGSTextureSet*>(textures)) {
			const char* id = record->GetFormEditorID();
			if (id && *id && std::string_view(id).size() <= 256)
				return id;
		}
		const auto signature = TextureSignature(textures);
		std::scoped_lock lock(textureMutex);
		const auto it = textureSources.find(textures);
		return !signature.empty() && it != textureSources.end() && !it->second.ambiguous && it->second.signature == signature ?
			it->second.editorId : std::string{};
	}

	void ReadNifParameters(const RE::BSLightingShaderProperty& property,
		std::optional<std::array<float, 4>>& thin, std::optional<std::array<float, 2>>& fuzz)
	{
		thin = ReadFloats<4>(property, "PBRThinSurface");
		fuzz = ReadFloats<2>(property, "PBRFuzz");
	}
}

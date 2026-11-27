#include "NativeMaterialMetadata.h"

#include "Hooks.h"
#include "RE/B/BSLightingShaderProperty.h"
#include "RE/B/BSShaderTextureSet.h"
#include "RE/B/BSTextureSet.h"
#include "RE/B/BGSTextureSet.h"
#include "RE/N/NiIntegersExtraData.h"
#include "RE/N/NiFloatsExtraData.h"
#include "Utils/StringUtils.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <mutex>
#include <list>
#include <unordered_map>
#include <vector>

namespace NativePBR
{
	namespace
	{
		constexpr auto kAuthoredFlags = "OS.NativePBR.OriginalFlags.v1";
		constexpr size_t kMaxTextureSources = 32768;

		// Engine form IDs are needed even when the legacy TruePBR feature is
		// not installed. Intern strings so GetFormEditorID returns stable memory.
		std::mutex editorMutex;
		std::list<std::string> editorStrings;
		std::unordered_map<uint32_t, const char*> editorIDs;
		template <class Form>
		struct GetEditorID
		{
			static const char* thunk(const RE::TESForm* form)
			{
				std::scoped_lock lock(editorMutex);
				const auto it = editorIDs.find(form->GetFormID());
				return it != editorIDs.end() ? it->second : "";
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		template <class Form>
		struct SetEditorID
		{
			static bool thunk(RE::TESForm* form, const char* editorId)
			{
				if (editorId) {
					std::scoped_lock lock(editorMutex);
					const auto id = form->GetFormID();
					const auto it = editorIDs.find(id);
					if (it == editorIDs.end() || std::string_view(it->second) != editorId) {
						editorStrings.emplace_back(editorId);
						editorIDs[id] = editorStrings.back().c_str();
					}
				}
				return func(form, editorId);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		template <class Form, class VTable>
		void InstallEditorID(const VTable& vtable)
		{
			stl::write_vfunc<0x32, GetEditorID<Form>>(vtable[0]);
			stl::write_vfunc<0x33, SetEditorID<Form>>(vtable[0]);
		}

		bool HasBaseTextures(const RE::BSLightingShaderProperty* property)
		{
			if (!property || !property->material ||
				property->material->GetType() != RE::BSShaderMaterial::Type::kLighting ||
				property->material->GetFeature() != RE::BSShaderMaterial::Feature::kDefault)
				return false;
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
			auto* textures = material->textureSet.get();
			const auto* expected = REL::Relocation<const RE::NiRTTI*>{ RE::BSTextureSet::Ni_RTTI }.get();
			if (!textures || !expected || !textures->GetRTTI() || !textures->GetRTTI()->IsKindOf(expected))
				return false;
			for (const uint32_t slot : { 0u, 1u, 5u }) {
				const auto* path = textures->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(slot));
				if (!path || !*path)
					return false;
			}
			return true;
		}

		struct NativeNifPostLink
		{
			static void thunk(RE::BSLightingShaderProperty* property, RE::NiStream& stream)
			{
				func(property, stream);
				using enum RE::BSShaderProperty::EShaderPropertyFlag;
				if (!property || !property->flags.any(kMenuScreen) ||
					property->flags.any(kLODLandscape, kMultiTextureLandscape, kFace, kHairTint, kTreeAnim,
						kRefraction, kTempRefraction, kVertexAlpha, kGrayscaleToPaletteAlpha) ||
					!HasBaseTextures(property))
					return;
				auto path = Util::ToLowerAscii(stream.inputFilePath);
				std::ranges::replace(path, '\\', '/');
				if (path.find("worldmap") != std::string::npos || path.find("mapmenu") != std::string::npos ||
					path.find("interface/") != std::string::npos)
					return;
				PreserveAuthoredFlags(*property);
				if (!ReadAuthoredFlags(*property))
					return;
				// Preserve the ordinary vanilla raster fallback, but do not let
				// it interpret authored PBR feature markers as legacy effects.
				property->flags.reset(kMenuScreen, kSpecular, kGlowMap, kEnvMap, kMultiLayerParallax,
					kSoftLighting, kRimLighting, kBackLighting, kAnisotropicLighting, kEffectLighting, kFitSlope);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct NativeTextureSetConversion
		{
			static RE::BSShaderTextureSet* thunk(RE::BGSTextureSet* record)
			{
				auto* result = func(record);
				RememberTextureSet(record, result);
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
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

	void InstallExclusiveMetadataHooks()
	{
		static std::once_flag once;
		std::call_once(once, [] {
			InstallEditorID<RE::TESLandTexture>(RE::VTABLE_TESLandTexture);
			InstallEditorID<RE::BGSTextureSet>(RE::VTABLE_BGSTextureSet);
			InstallEditorID<RE::BGSMaterialObject>(RE::VTABLE_BGSMaterialObject);
			InstallEditorID<RE::BGSLightingTemplate>(RE::VTABLE_BGSLightingTemplate);
			InstallEditorID<RE::TESWeather>(RE::VTABLE_TESWeather);
			stl::detour_thunk<NativeTextureSetConversion>(REL::RelocationID(20905, 21361));
			stl::write_vfunc<0x1E, NativeNifPostLink>(RE::VTABLE_BSLightingShaderProperty[0]);
		});
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

#include "NativeMaterialMetadata.h"

#include "Hooks.h"
#include "RE/B/BSLightingShaderProperty.h"
#include "RE/B/BSLightingShaderMaterialBase.h"
#include "RE/N/NiSourceTexture.h"
#include "RE/B/BSShaderTextureSet.h"
#include "RE/B/BSTextureSet.h"
#include "RE/B/BGSTextureSet.h"
#include "RE/N/NiIntegersExtraData.h"
#include "RE/N/NiFloatsExtraData.h"
#include "Utils/StringUtils.h"

#include <algorithm>
#include <atomic>
#include "Utils/TracyZones.h"
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
		constexpr auto kGrassMaterialSource="OS.NativePBR.GrassSource.v1";
		constexpr size_t kMaxTextureSources = 32768;

		// Engine form IDs are needed even when the legacy TruePBR feature is
		// not installed. Intern strings so GetFormEditorID returns stable memory.
		std::atomic<uint64_t> textureMetadataRevision{ 1 };
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
					textureMetadataRevision.fetch_add(1, std::memory_order_release);
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

		bool HasBaseTextures(const RE::BSLightingShaderProperty* property, bool requireDefault = true)
		{
			if (!property || !property->material ||
				property->material->GetType() != RE::BSShaderMaterial::Type::kLighting ||
				(requireDefault && property->material->GetFeature() != RE::BSShaderMaterial::Feature::kDefault))
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
					property->flags.any(kLODLandscape, kMultiTextureLandscape, kRefraction, kTempRefraction, kGrayscaleToPaletteAlpha) ||
					!HasBaseTextures(property, false))
					return;
				auto path = Util::ToLowerAscii(stream.inputFilePath);
				std::ranges::replace(path, '\\', '/');
				if (path.find("worldmap") != std::string::npos || path.find("mapmenu") != std::string::npos ||
					path.find("interface/") != std::string::npos)
					return;
				PreserveAuthoredFlags(*property);
				if (!ReadAuthoredFlags(*property))
					return;
				// Preserve specialized material metadata even when the engine
				// material is not one of the legacy-safe flag-normalization
				// candidates. Do not change that engine material's flags.
				if (property->flags.any(kFace, kHairTint, kTreeAnim, kVertexAlpha) || !HasBaseTextures(property))
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

	// Serialize the original NIF recipe into property-owned extra data.
	// Generated grass can outlive the source property and texture-set pointer.
	// No global pointer cache or legacy material replacement is needed.
	void PreserveGrassMaterialSource(const RE::BSLightingShaderProperty* source, RE::BSLightingShaderProperty* grass)
	{
		if (!source || !grass || grass->HasExtraData(kGrassMaterialSource) || !source->material ||
			source->material->GetType()!=RE::BSShaderMaterial::Type::kLighting) return;
		const auto flags=ReadAuthoredFlags(*source);
		if (!flags || !(*flags&static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kVertexLighting))) return;
		const auto* material=static_cast<const RE::BSLightingShaderMaterialBase*>(source->material);
		const auto snapshot=CaptureTextureSetSnapshot(material->textureSet.get());
		if (!snapshot || snapshot->oversized || snapshot->paths[0].empty() ||
			snapshot->paths[1].empty() || snapshot->paths[5].empty()) return;
		GrassMaterialSource data{*flags,*snapshot,{},{}};
		ReadNifParameters(*source,data.thin,data.fuzz);
		if (grass->material && grass->material->GetType()==RE::BSShaderMaterial::Type::kLighting) {
			const auto* generated=static_cast<const RE::BSLightingShaderMaterialBase*>(grass->material);
			const auto* path=generated->diffuseTexture ? generated->diffuseTexture->name.c_str() : nullptr;
			if (path && *path && std::string_view(path).size()<=260u) data.textures.paths[0]=path;
		}
		std::vector<int32_t> words{1,std::bit_cast<int32_t>(uint32_t(data.flags)),
			std::bit_cast<int32_t>(uint32_t(data.flags>>32u)),int32_t(bool(data.thin))|int32_t(bool(data.fuzz))*2};
		for (float value:data.thin.value_or(std::array<float,4>{})) words.push_back(std::bit_cast<int32_t>(value));
		for (float value:data.fuzz.value_or(std::array<float,2>{})) words.push_back(std::bit_cast<int32_t>(value));
		const auto append=[&](const std::string& path) {
			words.push_back(int32_t(path.size()));
			for (size_t i=0;i<path.size();i+=4u) {
				uint32_t word=0;
				for (size_t j=0;j<4u && i+j<path.size();++j) word|=uint32_t(uint8_t(path[i+j]))<<(8u*j);
				words.push_back(std::bit_cast<int32_t>(word));
			}
		};
		append(data.textures.editorId);
		for (const auto& path:data.textures.paths) append(path);
		RE::NiPointer<RE::NiIntegersExtraData> metadata{RE::NiIntegersExtraData::Create(kGrassMaterialSource,words)};
		if (metadata) grass->AddExtraData(metadata.get());
	}

	bool HasGrassMaterialSource(const RE::BSShaderProperty& property)
	{
		return property.HasExtraData(kGrassMaterialSource);
	}

	std::optional<GrassMaterialSource> ReadGrassMaterialSource(const RE::BSShaderProperty& property)
	{
		const auto* data=netimmerse_cast<const RE::NiIntegersExtraData*>(property.GetExtraData(kGrassMaterialSource));
		if (!data || !data->value || data->size<19u || data->size>604u || data->value[0]!=1) return {};
		GrassMaterialSource result;
		const auto* words=data->value;
		result.flags=uint64_t(uint32_t(words[1]))|(uint64_t(uint32_t(words[2]))<<32u);
		const uint32_t presence=uint32_t(words[3]);
		if (presence&~3u) return {};
		if (presence&1u) {
			std::array<float,4> v{};
			for (size_t i=0;i<v.size();++i) v[i]=std::bit_cast<float>(words[4u+i]);
			if (!std::ranges::all_of(v,[](float x){return std::isfinite(x);})) return {};
			result.thin=v;
		}
		if (presence&2u) {
			std::array<float,2> v{};
			for (size_t i=0;i<v.size();++i) v[i]=std::bit_cast<float>(words[8u+i]);
			if (!std::ranges::all_of(v,[](float x){return std::isfinite(x);})) return {};
			result.fuzz=v;
		}
		size_t cursor=10u;
		const auto read=[&](std::string& path) {
			if (cursor>=data->size) return false;
			const uint32_t length=uint32_t(words[cursor++]);
			if (length>260u || (length+3u)/4u>data->size-cursor) return false;
			path.resize(length);
			for (uint32_t i=0;i<length;++i) path[i]=char(uint32_t(words[cursor+i/4u])>>(8u*(i%4u)));
			cursor+=(length+3u)/4u;
			return path.find('\0')==std::string::npos;
		};
		if (!read(result.textures.editorId)) return {};
		for (auto& path:result.textures.paths) if (!read(path)) return {};
		if (cursor!=data->size || result.textures.paths[0].empty() ||
			result.textures.paths[1].empty() || result.textures.paths[5].empty()) return {};
		return result;
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
		bool changed = false;
		if (textureSources.size() >= kMaxTextureSources && !textureSources.contains(converted)) {
			textureSources.clear();
			changed = true;
		}
		auto& entry = textureSources[converted];
		if (entry.signature != signature) {
			entry = { std::move(signature), id, false };
			changed = true;
		} else if (entry.editorId != id && !entry.ambiguous) {
			entry.ambiguous = true;
			changed = true;
		}
		if (changed) textureMetadataRevision.fetch_add(1, std::memory_order_release);
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

	std::shared_ptr<const TextureSetSnapshot> CaptureTextureSetSnapshot(RE::BSTextureSet* textures)
	{
		CS_DRAW_ZONE("DX12::MaterialMetadata::TextureSnapshot");
		if (!textures) return {};
		struct Entry
		{
			uintptr_t identity = 0;
			uint64_t revision = 0;
			std::shared_ptr<const TextureSetSnapshot> snapshot;
		};
		// Thread-local engine acquisition cache. Only opaque identity values
		// and owned strings survive a call; workers consume the snapshots.
		static thread_local std::array<Entry, 1024> cache;
		const auto identity = reinterpret_cast<uintptr_t>(textures);
		auto& entry = cache[(identity >> 4u) % cache.size()];
		const auto revision = textureMetadataRevision.load(std::memory_order_acquire);
		std::array<std::string_view, 8> paths{};
		bool oversized = false;
		for (uint32_t i = 0; i < paths.size(); ++i) {
			const auto* path = textures->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i));
			if (path) paths[i] = path;
			oversized |= paths[i].size() > 260;
		}
		if (!oversized && entry.identity == identity && entry.revision == revision && entry.snapshot &&
			std::ranges::equal(paths, entry.snapshot->paths, [](std::string_view left, const std::string& right) { return left == std::string_view(right); }) &&
			textureMetadataRevision.load(std::memory_order_acquire) == revision)
			return entry.snapshot;
		auto result = std::make_shared<TextureSetSnapshot>();
		result->oversized = oversized;
		result->editorId = ResolveTextureSet(textures);
		for (size_t i = 0; i < paths.size(); ++i)
			if (paths[i].size() <= 260) result->paths[i] = paths[i];
		if (!oversized && textureMetadataRevision.load(std::memory_order_acquire) == revision)
			entry = { identity, revision, result };
		else entry = {};
		return result;
	}

	void ReadNifParameters(const RE::BSLightingShaderProperty& property,
		std::optional<std::array<float, 4>>& thin, std::optional<std::array<float, 2>>& fuzz)
	{
		thin = ReadFloats<4>(property, "PBRThinSurface");
		fuzz = ReadFloats<2>(property, "PBRFuzz");
	}
}

#include "SharedMaterialHooks.h"
#include "Features/InteriorSun.h"
#include "Features/NativePBR/MaterialCatalog.h"
#include "ShaderCache.h"
#include "Features/DX12/DX12Control.h"
#include "RE/B/BSTextureSet.h"
#include "Utils/StringUtils.h"
#include "Globals.h"
#include "TruePBR.h"

#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
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

	std::mutex textureMutex;
	struct TextureSource {
		std::string signature, editorId;
		bool ambiguous = false;
	};
	std::unordered_map<const RE::BSTextureSet*, TextureSource> textureSources;
	constexpr size_t kMaxTextureSources = 32768;

	std::string TextureSignature(RE::BSTextureSet* textures)
	{
		std::string key;
		for (uint32_t i = 0; i < 8; ++i) {
			const auto* path = textures->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i));
			auto name = Util::ToLowerAscii(path ? path : "");
			std::ranges::replace(name, '/', '\\');
			key += std::to_string(name.size()) + ":" + name;
		}
		return key;
	}

	struct NativeTextureSetConversion
	{
		static RE::BSShaderTextureSet* thunk(RE::BGSTextureSet* record)
		{
			auto* result = func(record);
			if (!record || !result) return result;
			const char* id = record->GetFormEditorID();
			if (!id || !*id) return result;
			auto signature = TextureSignature(result);
			std::scoped_lock lock(textureMutex);
			if (textureSources.size() >= kMaxTextureSources && !textureSources.contains(result))
				textureSources.clear();
			auto& entry = textureSources[result];
			if (entry.signature != signature)
				entry = { std::move(signature), id, false };
			else if (entry.editorId != id)
				entry.ambiguous = true;
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSLightingShader* shader, RE::BSRenderPass* pass, uint32_t renderFlags)
		{
			const auto originalTechnique = shader->currentRawTechnique;
	
			if (!globals::features::dx12Control.UsesExclusiveMaterials() &&
				(shader->currentRawTechnique & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::TruePbr)) != 0) {
				shader->currentRawTechnique |= static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::AmbientSpecular);
				shader->currentRawTechnique ^= static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::AnisoLighting);
			}
	
			shader->currentRawTechnique &= ~0b111000u;
			shader->currentRawTechnique |= (std::clamp(int(pass->numLights) - 1, 0, 7) << 3);
	
			func(shader, pass, renderFlags);
	
			shader->currentRawTechnique = originalTechnique;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	
	struct BSLightingShader_GetPixelTechnique
	{
		static uint32_t thunk(uint32_t rawTechnique)
		{
			uint32_t pixelTechnique = rawTechnique;
	
			pixelTechnique &= ~0b111000000u;
			if ((pixelTechnique & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::ModelSpaceNormals)) == 0) {
				pixelTechnique &= ~static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::Skinned);
			}
			pixelTechnique |= static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::VC);
	
			return pixelTechnique;
		}
	};

	struct PBR_BSLightingShader_SetupMaterial
	{
		static void thunk(RE::BSLightingShader* shader, RE::BSLightingShaderMaterialBase const* material)
		{
			if (!material) {
				return;
			}
	
			if (!globals::features::dx12Control.UsesExclusiveMaterials() && globals::features::truePBR.loaded &&
				globals::features::truePBR.BSLightingShader_SetupMaterial(shader, material)) {
				return;
			}
	
			// Vanilla dereferences these unconditionally; they are null when the texture set never loaded or was cleared.
			if ((material->diffuseRenderTargetSourceIndex == -1 && !material->diffuseTexture) || !material->normalTexture) {
				return;
			}
	
			func(shader, material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct LightingRenderPasses
	{
		static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSLightingShaderProperty* property,
			RE::BSGeometry* geometry, uint32_t flags, RE::BSShaderAccumulator* accumulator)
		{
			auto* passes = func(property, geometry, flags, accumulator);
			if (passes) {
				globals::features::truePBR.RewriteLightingRenderPasses(property, passes);
				globals::features::interiorSun.RewriteLightingRenderPasses(passes);
			}
			return passes;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void SharedMaterialHooks::Install()
{
	static std::once_flag once;
	std::call_once(once, [] {
		InstallEditorID<RE::TESLandTexture>(RE::VTABLE_TESLandTexture);
		InstallEditorID<RE::BGSTextureSet>(RE::VTABLE_BGSTextureSet);
		InstallEditorID<RE::BGSMaterialObject>(RE::VTABLE_BGSMaterialObject);
		InstallEditorID<RE::BGSLightingTemplate>(RE::VTABLE_BGSLightingTemplate);
		InstallEditorID<RE::TESWeather>(RE::VTABLE_TESWeather);
		stl::write_vfunc<0x2A, LightingRenderPasses>(RE::VTABLE_BSLightingShaderProperty[0]);
		const bool exclusive = globals::features::dx12Control.UsesExclusiveMaterials();
		if (exclusive)
			stl::detour_thunk<NativeTextureSetConversion>(REL::RelocationID(20905, 21361));
		if (exclusive || globals::features::truePBR.loaded) {
			stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
			stl::detour_thunk_ignore_func<BSLightingShader_GetPixelTechnique>(REL::RelocationID(101633, 108700));
			stl::write_vfunc<0x4, PBR_BSLightingShader_SetupMaterial>(RE::VTABLE_BSLightingShader[0]);
		}
	});
}

std::string SharedMaterialHooks::ResolveTextureSetEditorID(RE::BSTextureSet* textures)
{
	if (!textures) return {};
	if (const auto* record = skyrim_cast<RE::BGSTextureSet*>(textures)) {
		const char* id = record->GetFormEditorID();
		if (id && *id) return id;
	}
	const auto signature = TextureSignature(textures);
	std::scoped_lock lock(textureMutex);
	const auto it = textureSources.find(textures);
	return it != textureSources.end() && !it->second.ambiguous && it->second.signature == signature ?
		it->second.editorId : std::string{};
}

void SharedMaterialHooks::NormalizeRasterDescriptors(RE::BSShader::Type type, uint32_t& vertex, uint32_t& pixel)
{
	if (!globals::features::dx12Control.UsesExclusiveMaterials()) return;
	using Cache = SIE::ShaderCache;
	if (type == RE::BSShader::Type::Lighting) {
		const auto flag = static_cast<uint32_t>(Cache::LightingShaderFlags::TruePbr);
		vertex &= ~flag;
		pixel &= ~flag;
	} else if (type == RE::BSShader::Type::Utility) {
		const auto flag = static_cast<uint32_t>(Cache::UtilityShaderFlags::TruePbr);
		vertex &= ~flag;
		pixel &= ~flag;
	}
}

std::optional<std::array<float, 3>> SharedMaterialHooks::ProjectedColorScale(const RE::TESForm* form)
{
	if (!form) return {};
	if (globals::features::dx12Control.UsesExclusiveMaterials()) {
		const auto catalog = NativePBR::MaterialCatalog::Shared().GetSnapshot();
		if (catalog)
			if (const auto* record = catalog->FindMaterialObject(form)) return record->baseColorScale;
	} else if (globals::features::truePBR.loaded) {
		if (const auto* record = globals::features::truePBR.GetPBRMaterialObjectData(form))
			return record->baseColorScale;
	}
	return {};
}

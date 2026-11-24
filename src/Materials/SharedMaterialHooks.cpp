#include "SharedMaterialHooks.h"
#include "NativeRasterPolicy.h"
#include "Features/InteriorSun.h"
#include "Features/NativePBR/MaterialCatalog.h"
#include "ShaderCache.h"
#include "Features/DX12/DX12Control.h"
#include "RE/B/BSTextureSet.h"
#include "RE/N/NiIntegersExtraData.h"
#include "Utils/StringUtils.h"
#include "Globals.h"
#include "TruePBR.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"
#include "TruePBR/BSLightingShaderMaterialPBRLandscape.h"

#include <bit>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
	std::mutex editorMutex;
	std::atomic<bool> legacyNifMarkers{ false };
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

	constexpr auto kNativeNifFlags = "OS.NativePBR.OriginalFlags.v1";

	bool HasBasePbrTextures(const RE::BSLightingShaderProperty* property)
	{
		if (!property || !property->material || property->material->GetType() != RE::BSShaderMaterial::Type::kLighting ||
			property->material->GetFeature() != RE::BSShaderMaterial::Feature::kDefault) return false;
		const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
		auto* textures = material->textureSet.get();
		const auto* expected = REL::Relocation<const RE::NiRTTI*>{ RE::BSTextureSet::Ni_RTTI }.get();
		if (!textures || !expected || !textures->GetRTTI() || !textures->GetRTTI()->IsKindOf(expected)) return false;
		for (uint32_t slot : { 0u, 1u, 5u }) {
			const auto* path = textures->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(slot));
			if (!path || !*path) return false;
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
				!HasBasePbrTextures(property) || property->HasExtraData(kNativeNifFlags)) return;
			auto path = Util::ToLowerAscii(stream.inputFilePath);
			std::ranges::replace(path, '\\', '/');
			if (path.find("worldmap") != std::string::npos || path.find("mapmenu") != std::string::npos ||
				path.find("interface/") != std::string::npos) return;
			const auto original = static_cast<uint64_t>(property->flags.underlying());
			const std::vector<int32_t> words{
				std::bit_cast<int32_t>(uint32_t(original)),
				std::bit_cast<int32_t>(uint32_t(original >> 32))
			};
			RE::NiPointer<RE::NiIntegersExtraData> metadata{ RE::NiIntegersExtraData::Create(kNativeNifFlags, words) };
			if (!metadata || !property->AddExtraData(metadata.get())) return;
			// The native recipe retains the authored flags; vanilla raster must not interpret the repurposed bits.
			property->flags.reset(kMenuScreen, kSpecular, kGlowMap, kEnvMap, kMultiLayerParallax,
				kSoftLighting, kRimLighting, kBackLighting, kAnisotropicLighting, kEffectLighting, kFitSlope);
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

	bool EligibleNativeRaster(const RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry)
	{
		using F = RE::BSShaderProperty::EShaderPropertyFlag;
		const auto original = SharedMaterialHooks::OriginalNativePbrFlags(property);
		if (!original || !geometry || !geometry->AsTriShape() || geometry->AsDynamicTriShape() ||
			!HasBasePbrTextures(property) || property->alpha != 1.0f ||
			property->flags.any(F::kProjectedUV, F::kVertexAlpha, F::kRefraction, F::kTempRefraction,
				F::kTreeAnim, F::kLODLandscape, F::kMultiTextureLandscape, F::kFace, F::kHairTint)) return false;
		constexpr uint64_t specialized = static_cast<uint64_t>(F::kMultiLayerParallax) | static_cast<uint64_t>(F::kBackLighting) |
			static_cast<uint64_t>(F::kSoftLighting) | static_cast<uint64_t>(F::kRimLighting) |
			static_cast<uint64_t>(F::kAnisotropicLighting) | static_cast<uint64_t>(F::kEffectLighting) | static_cast<uint64_t>(F::kFitSlope);
		if (*original & specialized) return false;
		const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
		const auto& runtime = geometry->GetGeometryRuntimeData();
		using V = RE::BSGraphics::Vertex;
		if (material->materialAlpha != 1.0f || runtime.skinInstance || runtime.alphaProperty ||
			!runtime.vertexDesc.HasFlag(V::VF_UV) || !runtime.vertexDesc.HasFlag(V::VF_NORMAL) ||
			!runtime.vertexDesc.HasFlag(V::VF_TANGENT)) return false;
		for (auto* node = static_cast<RE::NiAVObject*>(geometry); node; node = node->parent) {
			if (const auto* owner = node->GetUserData())
				return !owner->IsDynamicForm() && !owner->IsActor();
		}
		return false;
	}

	void RouteNativeRasterPasses(RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry,
		RE::BSShaderProperty::RenderPassArray* passes)
	{
		if (!globals::features::dx12Control.UsesNativeRasterInputs()) return;
		const bool eligible = EligibleNativeRaster(property, geometry);
		using Flags = SIE::ShaderCache::LightingShaderFlags;
		constexpr uint32_t incompatible = uint32_t(Flags::Skinned) | uint32_t(Flags::ModelSpaceNormals) |
			uint32_t(Flags::ProjectedUV) | uint32_t(Flags::WorldMap) | uint32_t(Flags::DoAlphaTest) |
			uint32_t(Flags::AdditionalAlphaMask) | uint32_t(Flags::FurShells) | uint32_t(Flags::FurShellsDepth);
		constexpr uint32_t start = 0x4800002D;
		for (auto* pass = passes->head; pass; pass = pass->next) {
			if (!pass->shader || pass->shader->shaderType != RE::BSShader::Type::Lighting) continue;
			uint32_t descriptor = pass->passEnum - start;
			descriptor = NativeRasterPolicy::RoutePass(descriptor, eligible, incompatible);
			pass->passEnum = descriptor + start;
		}
	}

	struct LightingRenderPasses
	{
		static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSLightingShaderProperty* property,
			RE::BSGeometry* geometry, uint32_t flags, RE::BSShaderAccumulator* accumulator)
		{
			auto* passes = func(property, geometry, flags, accumulator);
			if (passes) {
				globals::features::truePBR.RewriteLightingRenderPasses(property, passes);
				globals::features::interiorSun.RewriteLightingRenderPasses(passes);
				RouteNativeRasterPasses(property, geometry, passes);
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
		legacyNifMarkers.store(!exclusive && globals::features::truePBR.loaded, std::memory_order_release);
		if (exclusive) {
			globals::features::truePBR.loaded = false;
			logger::info("[TruePBR] Disabled for exclusive DX12 material authority");
			stl::detour_thunk<NativeTextureSetConversion>(REL::RelocationID(20905, 21361));
			stl::write_vfunc<0x1E, NativeNifPostLink>(RE::VTABLE_BSLightingShaderProperty[0]);
		}
		if (exclusive || globals::features::truePBR.loaded) {
			stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
			stl::detour_thunk_ignore_func<BSLightingShader_GetPixelTechnique>(REL::RelocationID(101633, 108700));
			stl::write_vfunc<0x4, PBR_BSLightingShader_SetupMaterial>(RE::VTABLE_BSLightingShader[0]);
		}
	});
}

bool SharedMaterialHooks::UsesLegacyNifMarkers()
{
	return legacyNifMarkers.load(std::memory_order_acquire);
}

std::optional<uint64_t> SharedMaterialHooks::OriginalNativePbrFlags(const RE::BSLightingShaderProperty* property)
{
	if (!property) return {};
	const auto* data = netimmerse_cast<const RE::NiIntegersExtraData*>(property->GetExtraData(kNativeNifFlags));
	if (!data || data->size != 2 || !data->value) return {};
	const uint64_t flags = uint64_t(uint32_t(data->value[0])) | (uint64_t(uint32_t(data->value[1])) << 32);
	if (!(flags & static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kMenuScreen))) return {};
	return flags;
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

void SharedMaterialHooks::NormalizeRasterDescriptors(RE::BSShader::Type type, uint32_t& vertex, uint32_t& pixel, bool deferred)
{
	if (!globals::features::dx12Control.UsesExclusiveMaterials()) return;
	using Cache = SIE::ShaderCache;
	static_assert(uint32_t(Cache::LightingShaderFlags::TruePbr) == NativeRasterPolicy::kRouteBit);
	static_assert(uint32_t(Cache::LightingShaderFlags::NativePbrInput) == NativeRasterPolicy::kInputBit);
	if (type == RE::BSShader::Type::Lighting) {
		vertex &= ~(NativeRasterPolicy::kRouteBit | NativeRasterPolicy::kInputBit);
		pixel = NativeRasterPolicy::NormalizePixel(pixel, globals::features::dx12Control.UsesNativeRasterInputs(), deferred);
	} else if (type == RE::BSShader::Type::Utility) {
		const auto flag = static_cast<uint32_t>(Cache::UtilityShaderFlags::TruePbr);
		vertex &= ~flag;
		pixel &= ~flag;
	}
}

void SharedMaterialHooks::StripNativeRasterRoute(RE::BSShader::Type type, uint32_t& vertex, uint32_t& pixel)
{
	if (type != RE::BSShader::Type::Lighting || !globals::features::dx12Control.UsesNativeRasterInputs()) return;
	vertex &= ~(NativeRasterPolicy::kRouteBit | NativeRasterPolicy::kInputBit);
	pixel &= ~(NativeRasterPolicy::kRouteBit | NativeRasterPolicy::kInputBit);
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

SharedMaterialHooks::LegacyResidencyStats SharedMaterialHooks::LegacyResidency()
{
	LegacyResidencyStats stats;
	{
		std::scoped_lock lock(BSLightingShaderMaterialPBR::AllMutex);
		stats.meshes = BSLightingShaderMaterialPBR::All.size();
	}
	{
		std::scoped_lock lock(BSLightingShaderMaterialPBRLandscape::AllMutex);
		stats.landscapes = BSLightingShaderMaterialPBRLandscape::All.size();
	}
	stats.glints = bool(globals::features::truePBR.glintsNoiseTexture);
	return stats;
}

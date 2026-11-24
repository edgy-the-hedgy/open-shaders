#include "SharedMaterialHooks.h"
#include "Features/InteriorSun.h"
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
	});
}

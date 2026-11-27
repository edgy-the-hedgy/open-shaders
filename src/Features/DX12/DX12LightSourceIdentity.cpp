#include "DX12LightSourceIdentity.h"

#include "Features/LightLimitFix/ShadowCasterMath.h"
#include "RE/T/TESObjectLIGH.h"
#include "RE/T/TESObjectREFR.h"
#include "Utils/PointLightFlags.h"
#include "Utils/Game.h"

#include <Windows.h>
#include <cmath>

namespace DX12Native
{
	// Engine user data and parent pointers are not reference counted.
	void ReadFullLightOwner(const RE::NiLight* ni, uint32_t& owner, uint32_t& placed) noexcept
	{
		__try {
			if (auto* ref = Util::FindSceneOwner(ni)) {
				owner = ref->GetFormID();
				auto* base = ref->GetObjectReference();
				if (base && base->As<RE::TESObjectLIGH>())
					placed = owner;
			}
		} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
		}
	}
	void ReadFullLightRestPosition(RE::NiLight* ni, uint32_t placedReference, float3& position, uint32_t authoredFormID) noexcept
	{
		__try {
			const auto formID = authoredFormID ? authoredFormID : PointLightFlags::GetRuntimeLightFormID(ni);
			const auto* light = formID ? RE::TESForm::LookupByID<RE::TESObjectLIGH>(formID) : nullptr;
			if (!light && placedReference) {
				auto* reference = RE::TESForm::LookupByID<RE::TESObjectREFR>(placedReference);
				auto* base = reference ? reference->GetObjectReference() : nullptr;
				light = base ? base->As<RE::TESObjectLIGH>() : nullptr;
			}
			const auto& local = ni->local.translate;
			if (light && !light->GetNoFlicker() && ni->parent &&
				ShadowCasterManager::WithinFlickerReach(local.x, local.y, local.z, light->data.flickerMovementAmplitude)) {
				const auto& rest = ni->parent->world.translate;
				if (std::isfinite(rest.x) && std::isfinite(rest.y) && std::isfinite(rest.z))
					position = float3{ rest.x, rest.y, rest.z };
			}
		} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
		}
	}
}

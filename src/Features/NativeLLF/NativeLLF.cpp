#include "NativeLLF.h"
#include "NativeLLFParticleBridge.h"

#include "Features/DX12/DX12Control.h"
#include "Features/DX12/DX12LightMath.h"
#include "Features/DX12/DX12Resources.h"
#include "Features/LightLimitFix.h"
#include "Features/LightLimitFix/Common.h"
#include "Features/LightLimitFix/RadiusMath.h"
#include "Features/LightLimitFix/ShadowCasterMath.h"
#include "Globals.h"
#include "RE/T/TESObjectLIGH.h"
#include "State.h"
#include "Utils/PointLightFlags.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <unordered_set>

namespace DX12Native
{
	// Engine user data and parent pointers are not reference counted.
	void ReadFullLightOwner(const RE::NiLight* ni, uint32_t& owner, uint32_t& placed) noexcept
	{
		__try {
			const RE::NiAVObject* node = ni;
			for (uint32_t depth = 0; node && depth < 16; ++depth, node = node->parent) {
				if (auto* ref = node->GetUserData()) {
					owner = ref->GetFormID();
					auto* base = ref->GetObjectReference();
					if (base && base->As<RE::TESObjectLIGH>())
						placed = owner;
					return;
				}
			}
		} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
		}
	}
	void ReadFullLightRestPosition(RE::NiLight* ni, uint32_t placedReference, float3& position) noexcept
	{
		__try {
			const auto formID = PointLightFlags::GetRuntimeLightFormID(ni);
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

namespace
{
	struct ParticleEmitter
	{
		float3 position, color;
		float radius, fade;
	};

	// Render-thread CPU publication, identical lifetime to the old LLF snapshot.
	struct ParticleSnapshot
	{
		std::vector<ParticleEmitter> emitters;
		uint32_t frame = UINT32_MAX, pluginFrame = UINT32_MAX;
	};

	ParticleSnapshot& LegacyParticleSnapshot()
	{
		static ParticleSnapshot snapshot;
		return snapshot;
	}
}

void NativeLLF::ResetLegacyParticleBridge()
{
	auto& snapshot = LegacyParticleSnapshot();
	snapshot.emitters.clear();
	snapshot.frame = snapshot.pluginFrame = UINT32_MAX;
}

void NativeLLF::PublishLegacyParticleBridge(std::span<const LightLimitFix::LightData> lights,
	const RE::NiPoint3& eyePosition, uint32_t frame, uint32_t pluginFrame)
{
	auto& snapshot = LegacyParticleSnapshot();
	snapshot.emitters.clear();
	snapshot.emitters.reserve(lights.size());
	for (const auto& light : lights) {
		const auto& p = light.positionWS[0].data;
		snapshot.emitters.push_back({ { p.x + eyePosition.x, p.y + eyePosition.y, p.z + eyePosition.z },
			light.color, light.radius, light.fade });
	}
	snapshot.frame = frame;
	snapshot.pluginFrame = pluginFrame;
}

void NativeLLF::Producer::ResetParticleInput()
{
	ResetLegacyParticleBridge();
}

std::optional<NativeLLF::DirectInputFrame> NativeLLF::Producer::CollectDirectInputs(const DX12Native::TopLevel& scene, uint32_t frame, bool opaqueReady)
{
	if (!loaded || !globals::features::dx12Control.WantsMaterialPreparation() || !globals::state || !scene.inputs || !scene.inputs->residency)
		return std::nullopt;
	const auto& llf = globals::features::lightLimitFix;
	const auto& particles = LegacyParticleSnapshot();
	const bool particleSnapshotReady = llf.loaded && particles.frame == frame &&
		particles.pluginFrame == globals::state->frameCount;
	// Prefer early material dispatch where possible, but defer direct lighting to opaque-ready if
	// the current LLF emitter publication has not arrived yet.
	if (llf.loaded && !particleSnapshotReady && !opaqueReady)
		return std::nullopt;
	const auto prepared = DX12Resources::CollectPreparedLights(*scene.inputs->residency);
	DirectInputFrame result;
	result.frame = frame;
	result.scope = scene.scope;
	result.geometryRevision = scene.geometryRevision;
	auto& directLights = result.points;
	directLights.reserve(std::min(prepared.size(), size_t(DX12::kMaxPreparedLights)));
	const auto toLinear = [](float value) { return DX12Native::LightMath::ToLinear(value, false); };
	for (const auto& light : prepared) {
		if (directLights.size() >= DX12::kMaxPreparedLights)
			break;
		directLights.push_back({ light.position,
			{ toLinear(light.color.x) * light.fade, toLinear(light.color.y) * light.fade, toLinear(light.color.z) * light.fade },
			light.radius, light.emitterRadius, 0.0f, false });
		// A prepared spotlight without a current projection is not an omni light.
		if (light.flags & (static_cast<uint32_t>(RE::TES_LIGHT_FLAGS::kSpotlight) | static_cast<uint32_t>(RE::TES_LIGHT_FLAGS::kSpotShadow)))
			directLights.back().radius = 0.0f;
	}

	// Full Native samples live values without changing LLF, SCM, or their cached
	// shadow selections. Never call ProcessLight(), which mutates engine data.
	const auto node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
	bool runtimeAvailable = node != nullptr;
	if (node) {
		std::unordered_set<const RE::NiLight*> visited;
		std::vector<bool> matched(directLights.size(), false);
		const size_t preparedCount = directLights.size();
		const auto collect = [&](RE::BSLight* light) {
			if (!light || !light->pointLight || !light->light)
				return;
			auto* ni = light->light.get();
			if (!visited.insert(ni).second || ni->GetFlags().any(RE::NiAVObject::Flag::kHidden))
				return;
			const auto flags = PointLightFlags::GetVanillaPointLightFlags(light, ni);
			const auto runtimeFlags = PointLightFlags::GetRuntimeLightFlags(ni);
			using F = PointLightFlags::Flags;
			if (runtimeFlags & PointLightFlags::ToMask(F::Disabled))
				return;
			const auto& rt = ni->GetLightRuntimeData();
			const auto& position = ni->world.translate;
			if (!std::isfinite(rt.radius.x) || rt.radius.x <= 0.0f ||
				!std::isfinite(rt.fade) || rt.fade <= 0.0f ||
				!std::isfinite(rt.diffuse.red) || !std::isfinite(rt.diffuse.green) || !std::isfinite(rt.diffuse.blue) ||
				!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
				return;
			DX12Native::DirectPointLight value{ { position.x, position.y, position.z },
				{ toLinear(rt.diffuse.red) * rt.fade, toLinear(rt.diffuse.green) * rt.fade, toLinear(rt.diffuse.blue) * rt.fade },
				rt.radius.x, 0.0f, 0.0f, false };
			if (flags & PointLightFlags::ToMask(F::Spot)) {
				auto* spot = skyrim_cast<RE::BSShadowLight*>(light);
				std::array<float, 16> inverse{};
				if (!spot || !spot->GetIsFrustumLight() ||
					!ShadowCasterManager::GetLightProjection(spot, value.worldToSpot.data(), inverse.data()) ||
					!std::ranges::all_of(value.worldToSpot, [](float v) { return std::isfinite(v); }))
					return;
				value.spot = true;
			}
			if (runtimeFlags & PointLightFlags::ToMask(F::InverseSquare)) {
				const auto* ext = LLFCommon::RuntimeLightDataExt::Get(ni);
				if (!std::isfinite(ext->size) || ext->size <= 0.0f || !std::isfinite(ext->cutoffOverride) ||
					ext->cutoffOverride <= 0.0f)
					return;
				value.inverseSquare = true;
				value.radius = ISLMath::CalculateRadius(rt.fade * 4.0f,
					ShadowCasterManager::IsShadowLightType(light), ext->cutoffOverride, ext->size);
				value.fadeZone = 1.0f / std::min(value.radius, ISLMath::FadeZoneBase);
				value.sourceRadius = std::sqrt(ISLMath::ScaledUnitsSq * 0.5f) * ext->size;
				// DirectLighting uses a unitless reciprocal denominator; fold
				// LLF's intensity and unit conversion into the radiance record.
				const float intensity = 4.0f * ISLMath::ScaledUnitsSq;
				value.radiance.x *= intensity;
				value.radiance.y *= intensity;
				value.radiance.z *= intensity;
			}
			uint32_t owner = 0, placed = 0;
			DX12Native::ReadFullLightOwner(ni, owner, placed);
			auto rest = value.position;
			DX12Native::ReadFullLightRestPosition(ni, placed, rest);
			size_t best = preparedCount;
			float bestDistance = std::numeric_limits<float>::infinity();
			bool ambiguous = false;
			for (size_t i = 0; i < preparedCount; ++i) {
				const auto& candidate = prepared[i];
				const float dx = rest.x - candidate.position.x, dy = rest.y - candidate.position.y, dz = rest.z - candidate.position.z;
				const float d2 = dx * dx + dy * dy + dz * dz;
				if (!DX12Native::LightMath::SameEmitterCandidate(candidate.referenceID, placed, d2, candidate.radius))
					continue;
				if (d2 < bestDistance) {
					best = i;
					bestDistance = d2;
					ambiguous = false;
				} else if (d2 == bestDistance) {
					ambiguous = true;
				}
			}
			if (best != preparedCount) {
				if (!ambiguous && !matched[best]) {
					directLights[best] = value;
					matched[best] = true;
				}
				return;  // Never add a second representation of a prepared emitter.
			}
			if (directLights.size() < DX12::kMaxPreparedLights)
				directLights.push_back(value);
		};
		const auto& lights = node->GetRuntimeData();
		for (const auto& light : lights.activeShadowLights)
			collect(light.get());
		for (const auto& light : lights.activeLights)
			collect(light.get());
	}
	// Discard prepared spots for which no live projection was resolved, only
	// after matching so prepared ordinals remain stable during the merge.
	std::erase_if(directLights, [](const auto& light) { return light.radius <= 0.0f; });
	if (particleSnapshotReady) {
		for (const auto& particle : particles.emitters) {
			if (directLights.size() >= DX12::kMaxPreparedLights)
				break;
			if (!std::isfinite(particle.radius) || particle.radius <= 0.0f || !std::isfinite(particle.fade) || particle.fade <= 0.0f ||
				!std::isfinite(particle.position.x) || !std::isfinite(particle.position.y) || !std::isfinite(particle.position.z) ||
				!std::isfinite(particle.color.x) || !std::isfinite(particle.color.y) || !std::isfinite(particle.color.z))
				continue;
			directLights.push_back({ particle.position,
				{ toLinear(particle.color.x) * particle.fade, toLinear(particle.color.y) * particle.fade,
					toLinear(particle.color.z) * particle.fade }, particle.radius, 0.0f, 0.0f, false });
		}
	}
	auto& directional = result.directional;
	// Use the same directional values published by the existing raster
	// constant-buffer producer. Missing/late snapshots never fabricate light.
	float4 sunDirection{}, sunColor{};
	if (globals::state->HasDirectionalShadows() && globals::state->GetNativeDirectionalLight(frame, globals::state->frameCount, sunDirection, sunColor) &&
		std::isfinite(sunDirection.x) && std::isfinite(sunDirection.y) && std::isfinite(sunDirection.z) &&
		std::isfinite(sunColor.x) && std::isfinite(sunColor.y) && std::isfinite(sunColor.z)) {
		const float lengthSquared = sunDirection.x * sunDirection.x + sunDirection.y * sunDirection.y + sunDirection.z * sunDirection.z;
		if (lengthSquared > 1e-8f && std::isfinite(lengthSquared)) {
			const float inverseLength = 1.0f / std::sqrt(lengthSquared);
			directional.direction = { sunDirection.x * inverseLength, sunDirection.y * inverseLength, sunDirection.z * inverseLength, 1.0f };
			directional.radiance = { toLinear(sunColor.x), toLinear(sunColor.y), toLinear(sunColor.z), 0.0f };
		}
	}
	result.available = runtimeAvailable || !prepared.empty() || directional.direction.w != 0.0f;
	return result;
}

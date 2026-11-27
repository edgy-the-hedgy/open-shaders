#include "NativeLLF.h"

#include "Features/DX12/DX12Control.h"
#include "Features/DX12/DX12LightMath.h"
#include "Features/DX12/DX12LightSourceIdentity.h"
#include "Features/DX12/DX12Resources.h"
#include "Features/LightLimitFix.h"
#include "Features/LightLimitFix/Common.h"
#include "Features/LightLimitFix/RadiusMath.h"
#include "Features/LightLimitFix/ShadowCasterMath.h"
#include "Globals.h"
#include "RE/T/TESObjectLIGH.h"
#include "State.h"
#include "ShaderCache.h"
#include "Util.h"
#include "Utils/PointLightFlags.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <unordered_set>

void NativeLLF::Producer::PostPostLoad()
{
	// Inherit authored particle settings at boot; native acquisition owns its
	// own INI catalog, observation cache, cluster records and lifetime.
	particleSettings = globals::features::lightLimitFix.settings;
	configs.GetConfigs();
}

void NativeLLF::Producer::Reset()
{
	// The hook observes effects throughout the frame, including after opaque
	// lighting. Publish copied values at the next frame boundary, never pointers.
	current.swap(pending);
	pending.clear();
	observed.clear();
	{
		std::scoped_lock lock(referenceMutex);
		references.clear();
	}
	// State::Reset increments the plugin frame after feature Reset callbacks.
	currentPluginFrame = globals::state ? globals::state->frameCount + 1u : UINT32_MAX;
}

void NativeLLF::Producer::ResetParticleInput()
{
	pending.clear();
	current.clear();
	observed.clear();
	{
		std::scoped_lock lock(referenceMutex);
		references.clear();
	}
	currentPluginFrame = UINT32_MAX;
}

void NativeLLF::Producer::ObserveParticlePass(RE::BSRenderPass* pass)
{
	if (!loaded || !globals::features::dx12Control.WantsMaterialPreparation() ||
		!particleSettings.EnableParticleLights || !pass || !pass->geometry || !pass->shaderProperty ||
		!globals::shaderCache || !globals::shaderCache->IsEnabled() || pending.size() >= DX12::kMaxPreparedLights)
		return;
	if (pass->shaderProperty->GetRTTI() != globals::rtti::BSEffectShaderPropertyRTTI.get())
		return;
	if (!observed.insert(pass->geometry).second)
		return;
	const auto reference = ParticleInput::Recognize(pass, true, configs, references, referenceMutex);
	if (!reference.valid)
		return;
	if (const auto emitter = ParticleInput::ReadEmitter(pass, reference))
		CaptureEmitter(*emitter);
}

void NativeLLF::Producer::CaptureEmitter(const LightLimitFix::ParticleLightInfo& emitter)
{
	if (!emitter.node)
		return;
	const auto finite = [](float3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
	const auto append = [&](float3 position, float3 color, float radius) {
		if (pending.size() >= DX12::kMaxPreparedLights || !finite(position) || !finite(color) ||
			!std::isfinite(radius) || radius <= 1e-4f || color.x + color.y + color.z <= 1e-4f)
			return;
		pending.push_back({ position, color, radius });
	};
	const auto tint = [&](float3 color) {
		color = LightLimitFix::Saturation(color, particleSettings.ParticleLightsSaturation);
		Feature::ApplyPointLightColorOverrides(color);
		return color;
	};
	const float3 base{ emitter.color.red, emitter.color.green, emitter.color.blue };
	if (emitter.billboard) {
		const auto& p = emitter.node->world.translate;
		append({ p.x, p.y, p.z }, tint(base) * emitter.color.alpha * particleSettings.BillboardBrightness,
			emitter.node->worldBound.radius * emitter.radiusMult * particleSettings.BillboardRadius * 0.5f);
		return;
	}

	auto* system = static_cast<RE::NiParticleSystem*>(emitter.node);
	const auto data = system->GetParticlesRuntimeData().particleData;
	if (!data)
		return;
	const auto& runtime = data->GetParticlesRuntimeData();
	if (!runtime.radii || !runtime.sizes || !runtime.positions || !runtime.maxNumVertices)
		return;
	auto count = std::min(uint32_t(data->GetActiveVertexCount()), uint32_t(runtime.maxNumVertices));
	if (runtime.numVertices)
		count = std::min(count, uint32_t(runtime.numVertices));
	count = std::min(count, uint32_t(std::max(1, particleSettings.MaxParticlesPerEmitter)));

	// Spatially coherent clusters per emitter: never merge unrelated emitters.
	// Keep sums for radiance and means for position/radius, as in legacy LLF.
	ParticleEmitter cluster{};
	uint32_t members = 0;
	const auto flush = [&] {
		if (members) {
			append(cluster.position / float(members), cluster.color, cluster.radius / float(members));
			cluster = {};
			members = 0;
		}
	};
	for (uint32_t index = 0; index < count && pending.size() < DX12::kMaxPreparedLights; ++index) {
		auto p = runtime.positions[index];
		if (!system->GetParticleSystemRuntimeData().isWorldspace) {
			const auto& origin = emitter.node->GetModelData().modelBound.radius * emitter.node->world.scale != emitter.node->worldBound.radius ?
				emitter.node->worldBound.center : emitter.node->world.translate;
			p = { p.x + origin.x, p.y + origin.y, p.z + origin.z };
		}
		const float3 position{ p.x, p.y, p.z };
		const float radius = runtime.radii[index] * runtime.sizes[index] * emitter.radiusMult * particleSettings.ParticleRadius;
		float alpha = emitter.color.alpha;
		auto color = base;
		if (runtime.color) {
			alpha *= runtime.color[index].alpha;
			color.x *= runtime.color[index].red;
			color.y *= runtime.color[index].green;
			color.z *= runtime.color[index].blue;
		}
		color = tint(color) * alpha * particleSettings.ParticleBrightness;
		if (!finite(position) || !finite(color) || !std::isfinite(radius) || radius <= 0.0f)
			continue;
		if (members) {
			const auto delta = position - cluster.position / float(members);
			const float distance = std::sqrt(delta.Dot(delta));
			if (!particleSettings.EnableParticleLightsOptimization ||
				distance + std::abs(radius - cluster.radius / float(members)) > particleSettings.ParticleClusterThreshold)
				flush();
		}
		cluster.position += position;
		cluster.color += color;
		cluster.radius += radius;
		++members;
	}
	flush();
}

std::optional<NativeLLF::DirectInputFrame> NativeLLF::Producer::CollectDirectInputs(const DX12Native::TopLevel& scene, uint32_t frame, bool opaqueReady)
{
	if (!loaded || !globals::features::dx12Control.WantsMaterialPreparation() || !globals::state || !scene.inputs || !scene.inputs->residency)
		return std::nullopt;
	// Native particles are published by the frame reset, independently of
	// raster LLF prepass timing. Opaque-ready remains part of the caller contract.
	const bool particleSnapshotReady = currentPluginFrame == globals::state->frameCount;
	if (particleSettings.EnableParticleLights && !particleSnapshotReady && !opaqueReady)
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
		const auto eye = Util::GetEyePosition(0);
		const float fadeStart = RE::Sky::GetLightLODStartFadeSquared();
		const float fadeEnd = RE::Sky::GetLightLODEndFadeSquared();
		for (const auto& particle : current) {
			if (directLights.size() >= DX12::kMaxPreparedLights)
				break;
			const float3 offset{ particle.position.x - eye.x, particle.position.y - eye.y, particle.position.z - eye.z };
			const float distanceSq = offset.Dot(offset);
			const float maxDistance = particleSettings.MaxParticleDistance;
			if (maxDistance > 0.0f && distanceSq > maxDistance * maxDistance)
				continue;
			const float distance = distanceSq - particle.radius * particle.radius;
			float fade = 1.0f;
			if (fadeEnd > fadeStart && fadeEnd != 0.0f && distance >= fadeStart)
				fade = std::clamp(1.0f - (distance - fadeStart) / (fadeEnd - fadeStart), 0.0f, 1.0f);
			if (!std::isfinite(fade) || fade <= 0.0f)
				continue;
			directLights.push_back({ particle.position,
				{ toLinear(particle.color.x) * fade, toLinear(particle.color.y) * fade,
					toLinear(particle.color.z) * fade }, particle.radius, 0.0f, 0.0f, false });
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

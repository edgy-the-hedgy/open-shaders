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
#include "RE/T/TESObjectREFR.h"
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

namespace
{
	using NativeLLF::Policy::AuthoredLight;
	AuthoredLight ReadAuthoredLight(const RE::TESObjectLIGH& light)
	{
		return NativeLLF::Policy::MakeAuthoredLight(light.formID,
			light.data.flags.any(static_cast<RE::TES_LIGHT_FLAGS>(LLFCommon::TES_LIGHT_FLAGS_EXT::kInverseSquare)),
			light.data.flags.any(RE::TES_LIGHT_FLAGS::kSpotlight, RE::TES_LIGHT_FLAGS::kSpotShadow),
			light.data.fallofExponent, light.data.fov);
	}

	std::optional<AuthoredLight> ReadPlacedLight(uint32_t placed)
	{
		auto* reference = placed ? RE::TESForm::LookupByID<RE::TESObjectREFR>(placed) : nullptr;
		auto* base = reference ? reference->GetObjectReference() : nullptr;
		const auto* light = base ? base->As<RE::TESObjectLIGH>() : nullptr;
		return light ? std::optional{ReadAuthoredLight(*light)} : std::nullopt;
	}

	void ApplyInverseSquare(DX12Native::DirectPointLight& value, float fade, bool shadowCaster, float cutoff, float size)
	{
		value.inverseSquare = true;
		value.radius = ISLMath::CalculateRadius(fade * 4.0f, shadowCaster, cutoff, size);
		value.fadeZone = 1.0f / std::min(value.radius, ISLMath::FadeZoneBase);
		value.sourceRadius = std::sqrt(ISLMath::ScaledUnitsSq * 0.5f) * size;
		value.radiance *= 4.0f * ISLMath::ScaledUnitsSq;
	}
}

void NativeLLF::Producer::ObserveLightCreation(RE::NiLight* light, const RE::TESObjectLIGH& authored)
{
	if (!light || !globals::features::dx12Control.WantsMaterialPreparation())
		return;
	try {
		std::scoped_lock lock(metadataMutex);
		if (metadata.size() >= Policy::kMetadataCapacityMultiplier * DX12::kMaxPreparedLights && !metadata.contains(light)) {
			++metadataRejected;
			return;
		}
		metadata.insert_or_assign(light, MetadataEntry{RE::NiPointer<RE::NiLight>(light), ReadAuthoredLight(authored), metadataFrame});
	} catch (const std::exception& e) {
		logger::warn("[NativeLLF] Authored light observation failed: {}", e.what());
	}
}

void NativeLLF::Producer::BeginMetadataFrame()
{
	std::scoped_lock lock(metadataMutex);
	++metadataFrame;
	if (metadataFrame % Policy::kMetadataRetentionFrames == 0)
		std::erase_if(metadata, [&](const auto& entry) { return Policy::Expired(metadataFrame, entry.second.lastSeen); });
}

std::optional<NativeLLF::Policy::AuthoredLight> NativeLLF::Producer::FindMetadata(RE::NiLight* light, uint32_t placed)
{
	{
		std::scoped_lock lock(metadataMutex);
		if (auto it = metadata.find(light); it != metadata.end()) {
			it->second.lastSeen = metadataFrame;
			return it->second.authored;
		}
	}
	// Placed sources can be resolved even if they predate hook installation.
	if (auto authored = ReadPlacedLight(placed))
		return authored;
	// Compatibility for pre-existing attached lights while LLF is installed.
	if (globals::features::lightLimitFix.loaded) {
		const auto id = PointLightFlags::GetRuntimeLightFormID(light);
		if (const auto* form = id ? RE::TESForm::LookupByID<RE::TESObjectLIGH>(id) : nullptr)
			return ReadAuthoredLight(*form);
	}
	return std::nullopt;
}

void NativeLLF::Producer::OnRuntimeDisabled()
{
	ResetParticleInput();
	std::scoped_lock lock(metadataMutex);
	metadata.clear();
	metadataFrame = 0;
	metadataRejected = 0;
}

json NativeLLF::Producer::GetDiagnostics()
{
	std::scoped_lock lock(metadataMutex, observationMutex);
	return { { "authoredLightRecords", metadata.size() }, { "authoredLightRejected", metadataRejected },
		{ "particleSnapshotFrame", currentPluginFrame }, { "particleEmitters", current.size() } };
}

void NativeLLF::Producer::PostPostLoad()
{
	// Inherit authored particle settings at boot; native acquisition owns its
	// own INI catalog, observation cache, cluster records and lifetime.
	particleSettings = globals::features::lightLimitFix.settings;
	configs.GetConfigs();
	LightLimitFix::InstallLightCreationHook();
}

void NativeLLF::Producer::Reset()
{
	// The hook observes effects throughout the frame, including after opaque
	// lighting. Publish copied values at the next frame boundary, never pointers.
	std::scoped_lock observationLock(observationMutex);
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
	std::scoped_lock observationLock(observationMutex);
	pending.clear();
	current.clear();
	observed.clear();
	{
		std::scoped_lock lock(referenceMutex);
		references.clear();
	}
	currentPluginFrame = UINT32_MAX;
}

std::optional<ParticleInput::Observation> NativeLLF::Producer::ObserveParticlePass(RE::BSRenderPass* pass)
{
	if (!loaded || !globals::features::dx12Control.WantsMaterialPreparation() ||
		!particleSettings.EnableParticleLights || !pass || !pass->geometry || !pass->shaderProperty ||
		!globals::shaderCache || !globals::shaderCache->IsEnabled() ||
		!globals::state || globals::state->isMapMenuOpen || globals::features::dx12Resources.IsLoading() ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return std::nullopt;
	if (pass->shaderProperty->GetRTTI() != globals::rtti::BSEffectShaderPropertyRTTI.get())
		return std::nullopt;
	std::scoped_lock observationLock(observationMutex);
	if (pending.size() >= DX12::kMaxPreparedLights || observed.size() >= 16u * DX12::kMaxPreparedLights)
		return std::nullopt;
	const bool firstObservation = observed.insert(pass->geometry).second;
	ParticleInput::Observation result;
	result.reference = ParticleInput::Recognize(pass, true, configs, references, referenceMutex);
	if (result.reference.valid)
		result.emitter = ParticleInput::ReadEmitter(pass, result.reference);
	if (firstObservation && result.emitter)
		CaptureEmitter(*result.emitter);
	return result;
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
	bool particleSnapshotReady = false;
	{
		std::scoped_lock observationLock(observationMutex);
		particleSnapshotReady = currentPluginFrame == globals::state->frameCount;
	}
	if (particleSettings.EnableParticleLights && !particleSnapshotReady && !opaqueReady)
		return std::nullopt;
	BeginMetadataFrame();
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
		else if (light.flags & static_cast<uint32_t>(LLFCommon::TES_LIGHT_FLAGS_EXT::kInverseSquare)) {
			const auto authored = ReadPlacedLight(light.referenceID);
			if (authored && authored->inverseSquare)
				ApplyInverseSquare(directLights.back(), light.fade, false, authored->cutoff, authored->size);
			else
				directLights.back().radius = 0.0f; // Never silently reinterpret an unresolved ISL source.
		}
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
			if (!visited.insert(ni).second)
				return;
			uint32_t owner = 0, placed = 0;
			DX12Native::ReadFullLightOwner(ni, owner, placed);
			const auto& position = ni->world.translate;
			float3 rest{ position.x, position.y, position.z };
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
			// A hidden, disabled, or invalid live source must not leave its prepared
			// proxy emitting stale radiance. Never erase a previously admitted live match.
			const auto reject = [&] {
				if (best != preparedCount && !ambiguous && !matched[best])
					directLights[best].radius = 0.0f;
			};
			using F = PointLightFlags::Flags;
			const auto runtimeFlags = globals::features::lightLimitFix.loaded ? PointLightFlags::GetRuntimeLightFlags(ni) : 0u;
			const auto authored = FindMetadata(ni, placed);
			if (ni->GetFlags().any(RE::NiAVObject::Flag::kHidden) || (runtimeFlags & PointLightFlags::ToMask(F::Disabled))) {
				reject();
				return;
			}
			const auto& rt = ni->GetLightRuntimeData();
			if (!std::isfinite(rt.radius.x) || rt.radius.x <= 0.0f ||
				!std::isfinite(rt.fade) || rt.fade <= 0.0f ||
				!std::isfinite(rt.diffuse.red) || !std::isfinite(rt.diffuse.green) || !std::isfinite(rt.diffuse.blue) ||
				!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
				reject();
				return;
			}
			DX12Native::DirectPointLight value{ { position.x, position.y, position.z },
				{ toLinear(rt.diffuse.red) * rt.fade, toLinear(rt.diffuse.green) * rt.fade, toLinear(rt.diffuse.blue) * rt.fade },
				rt.radius.x, 0.0f, 0.0f, false };
			const bool spotSource = (authored && authored->spot) ||
				((runtimeFlags | PointLightFlags::GetPointLightTypeFlags(light)) & PointLightFlags::ToMask(F::Spot));
			if (spotSource) {
				auto* spot = skyrim_cast<RE::BSShadowLight*>(light);
				std::array<float, 16> inverse{};
				if (!spot || !spot->GetIsFrustumLight() ||
					!ShadowCasterManager::GetLightProjection(spot, value.worldToSpot.data(), inverse.data()) ||
					!std::ranges::all_of(value.worldToSpot, [](float v) { return std::isfinite(v); })) {
					reject();
					return;
				}
				value.spot = true;
			}
			const bool hasLegacyMetadata = (runtimeFlags & PointLightFlags::ToMask(F::Initialised)) != 0;
			const bool inverseSquare = hasLegacyMetadata ? (runtimeFlags & PointLightFlags::ToMask(F::InverseSquare)) != 0 :
				(authored && authored->inverseSquare);
			if (inverseSquare) {
				const auto* ext = hasLegacyMetadata ? LLFCommon::RuntimeLightDataExt::Get(ni) : nullptr;
				const float size = ext ? ext->size : authored->size;
				const float cutoff = ext ? ext->cutoffOverride : authored->cutoff;
				if (!std::isfinite(size) || size <= 0.0f || !std::isfinite(cutoff) || cutoff <= 0.0f) {
					reject();
					return;
				}
				ApplyInverseSquare(value, rt.fade, ShadowCasterManager::IsShadowLightType(light), cutoff, size);
			}
			if (best != preparedCount) {
				if (!ambiguous && !matched[best]) {
					directLights[best] = value;
					matched[best] = true;
				}
				return; // Never add a second representation of a prepared emitter.
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
	{
		std::scoped_lock observationLock(observationMutex);
		if (particleSnapshotReady && currentPluginFrame == globals::state->frameCount) {
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
	result.available = runtimeAvailable || !directLights.empty() || directional.direction.w != 0.0f;
	return result;
}

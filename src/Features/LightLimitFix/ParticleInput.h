#pragma once

#include "Features/LightLimitFix.h"
#include <optional>

namespace ParticleInput
{
	using ReferenceCache = eastl::hash_map<RE::BSGeometry*, LightLimitFix::ParticleLightReference>;

	// Engine observation and INI interpretation are shared; queues, light expansion,
	// renderer resources and feature state remain owned by the selected consumer.
	LightLimitFix::ParticleLightReference Recognize(RE::BSRenderPass* pass, bool enabled,
		ParticleLights& configs, ReferenceCache& cache, std::mutex& mutex);
	struct Observation
	{
		LightLimitFix::ParticleLightReference reference;
		std::optional<LightLimitFix::ParticleLightInfo> emitter;
	};

	std::optional<LightLimitFix::ParticleLightInfo> ReadEmitter(RE::BSRenderPass* pass,
		const LightLimitFix::ParticleLightReference& reference);
}

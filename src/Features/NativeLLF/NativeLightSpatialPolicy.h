#pragma once

#include "Features/DX12/NativeLightPublication.h"

namespace NativeLLF::SpatialPolicy
{
	struct Position { float x = 0, y = 0, z = 0; bool operator==(const Position&) const = default; };
	struct Source
	{
		Position position;
		uint32_t referenceID = 0;
		uint64_t runtimeIdentity = 0;
		bool operator==(const Source&) const = default;
	};
	struct Signature
	{
		uint32_t scope = 0;
		uint64_t revision = 0;
		DX12Native::NativeLightSettings settings;
		std::vector<Source> prepared, runtime;
	};
	inline bool SameSignature(const Signature& a, const Signature& b)
	{
		return a.scope == b.scope && a.revision == b.revision && a.settings == b.settings &&
			a.prepared == b.prepared && a.runtime == b.runtime;
	}
	struct Publication
	{
		Signature signature;
		DX12Native::FullShadowGrouping grouping;
	};
	inline void BuildGrouping(Publication& publication)
	{
		// The shared baseline algorithm needs an emission record; only its
		// membership result is retained. Radiance is evaluated on the render frame.
		struct Record : Source
		{
			Position color{};
			float radius = 0, emitterRadius = 0, fade = 1;
			uint32_t unifiedCount = 1;
		};
		const auto convert = [](const auto& sources) {
			std::vector<Record> result;
			result.reserve(sources.size());
			for (const auto& source : sources) { Record record; static_cast<Source&>(record) = source; result.push_back(record); }
			return result;
		};
		const auto& s = publication.signature;
		const auto prepared = convert(s.prepared), runtime = convert(s.runtime);
		(void)DX12Native::ConsolidateFullShadowLights<Record>(prepared, runtime, s.settings.unify, s.settings.exclude,
			s.settings.additive, &publication.grouping);
	}
	// Ordinary linear point lights only. Special attenuation models, spots and
	// particles bypass this policy and remain frame-local.
	template <class Light>
	std::vector<Light> ApplyGrouping(const DX12Native::FullShadowGrouping& grouping,
		std::span<const Light> prepared, std::span<const Light> runtime)
	{
		const auto toFloat = [](double value) { return float(std::min(value, double(std::numeric_limits<float>::max()))); };
		std::vector<Light> result(prepared.begin(), prepared.end());
		result.reserve(prepared.size() + grouping.runtimeGroups.size());
		for (size_t i = 0; i < grouping.preparedRuntime.size(); ++i) {
			const auto& group = grouping.preparedRuntime[i];
			if (group.empty()) continue;
			auto& anchor = result[i];
			double r = std::max(0.0f, anchor.radiance.x), g = std::max(0.0f, anchor.radiance.y), b = std::max(0.0f, anchor.radiance.z);
			for (auto index : group) {
				const auto& light = runtime[index];
				r += std::max(0.0f, light.radiance.x); g += std::max(0.0f, light.radiance.y); b += std::max(0.0f, light.radiance.z);
			}
			anchor.radiance = { toFloat(r), toFloat(g), toFloat(b) };
		}
		for (const auto& group : grouping.runtimeGroups) {
			if (group.empty()) continue;
			auto merged = runtime[group.front()];
			if (group.size() > 1) {
				double r = 0, g = 0, b = 0, weightSum = 0, x = 0, y = 0, z = 0, px = 0, py = 0, pz = 0;
				for (auto index : group) {
					const auto& light = runtime[index];
					const double red = std::max(0.0f, light.radiance.x), green = std::max(0.0f, light.radiance.y), blue = std::max(0.0f, light.radiance.z);
					const double weight = .2126 * red + .7152 * green + .0722 * blue;
					r += red; g += green; b += blue; weightSum += weight;
					x += weight * light.position.x; y += weight * light.position.y; z += weight * light.position.z;
					px += light.position.x; py += light.position.y; pz += light.position.z;
					merged.radius = std::max(merged.radius, light.radius);
					merged.sourceRadius = std::max(merged.sourceRadius, light.sourceRadius);
				}
				const double n = double(group.size());
				merged.radiance = { toFloat(r), toFloat(g), toFloat(b) };
				merged.position = { float(weightSum > 0 ? x / weightSum : px / n), float(weightSum > 0 ? y / weightSum : py / n), float(weightSum > 0 ? z / weightSum : pz / n) };
			}
			result.push_back(merged);
		}
		return result;
	}
}

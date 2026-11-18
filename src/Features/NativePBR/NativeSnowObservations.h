#pragma once

#include "NativeSnowSource.h"
#include "Utils/StringUtils.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace NativePBR
{
	/** @brief Bounded, engine-pointer-free observations keyed by scene reference and geometry name. */
	class NativeSnowObservations
	{
	public:
		static constexpr size_t kMaximumObservations = 8192;
		static constexpr uint32_t kTextureProbeIntervalFrames = 180;
		struct Snapshot
		{
			uint64_t revision = 0, identityRevision = 0, epoch = 0;
			std::unordered_map<std::string, SnowSourceState> entries;
		};

		static std::string Key(std::string_view a_reference, std::string_view a_shape)
		{
			auto result = Util::ToLowerAscii(a_reference);
			result.push_back('\0');
			result += Util::ToLowerAscii(a_shape);
			return result;
		}

		/** @brief Observes a pre-normalization render-pass descriptor and unmodified NIF property bits. */
		bool Observe(std::string_view a_reference, std::string_view a_shape, uint32_t a_passDescriptor, uint64_t a_nifFlags)
		{
			if (a_reference.empty() || a_shape.empty() || a_reference.size() > 256 || a_shape.size() > 1024)
				return false;
			auto key = Key(a_reference, a_shape);
			std::scoped_lock lock(mutex);
			if (!observed.entries.contains(key) && observed.entries.size() >= kMaximumObservations)
				return false;
			auto& entry = observed.entries[std::move(key)];
			const auto previous = entry;
			if (entry.runtimeNifFlags && *entry.runtimeNifFlags != a_nifFlags)
				entry.conflictingPasses = true;
			else
				entry.runtimeNifFlags = a_nifFlags;
			if (entry.renderPassDescriptor && !SnowPassesAgree(*entry.renderPassDescriptor, a_passDescriptor))
				entry.conflictingPasses = true;
			else if (!entry.renderPassDescriptor)
				entry.renderPassDescriptor = a_passDescriptor;
			if (entry == previous)
				return false;
			++observed.revision;
			++observed.identityRevision;
			return true;
		}

		/** @brief Accepts only explicitly verified DDS identities from the projected shader bindings. */
		bool ObserveProjectedTextures(std::string_view a_reference, std::string_view a_shape,
			const std::array<ObservedSnowTexture, kSnowProjectedTextureCount>& a_textures)
		{
			if (a_reference.empty() || a_shape.empty())
				return false;
			std::scoped_lock lock(mutex);
			const auto found = observed.entries.find(Key(a_reference, a_shape));
			if (found == observed.entries.end())
				return false;
			auto& entry = found->second;
			const auto previous = entry;
			for (size_t i = 0; i < a_textures.size(); ++i) {
				if (a_textures[i].canonicalPath.empty() && !a_textures[i].sampler)
					continue;
				const auto& incoming = a_textures[i];
				auto& stored = entry.projectedTextures[i];
				if ((!incoming.canonicalPath.empty() && !stored.canonicalPath.empty() &&
					(stored.canonicalPath != incoming.canonicalPath ||
						(stored.srgb && incoming.srgb && stored.srgb != incoming.srgb))) ||
					(stored.sampler && incoming.sampler && stored.sampler != incoming.sampler))
					entry.conflictingPasses = true;
				else {
					if (!incoming.canonicalPath.empty()) {
						stored.canonicalPath = incoming.canonicalPath;
						if (incoming.srgb)
							stored.srgb = incoming.srgb;
					}
					if (incoming.sampler)
						stored.sampler = incoming.sampler;
				}
			}
			if (entry == previous)
				return false;
			++observed.revision;
			if (entry.conflictingPasses != previous.conflictingPasses)
				++observed.identityRevision;
			return true;
		}

		/** @brief Tracks per-frame constants and flags contradictory passes within one frame. */
		bool ObserveProjection(std::string_view a_reference, std::string_view a_shape,
			const ObservedSnowProjection& a_projection)
		{
			if (a_reference.empty() || a_shape.empty())
				return false;
			std::scoped_lock lock(mutex);
			const auto found = observed.entries.find(Key(a_reference, a_shape));
			if (found == observed.entries.end())
				return false;
			auto& entry = found->second;
			if (entry.projection) {
				auto sameValues = a_projection;
				sameValues.frame = entry.projection->frame;
				if (*entry.projection == sameValues)
					return false;
			}
			if (entry.projection && entry.projection->frame == a_projection.frame &&
				*entry.projection != a_projection) {
				if (entry.conflictingPasses)
					return false;
				entry.conflictingPasses = true;
				++observed.identityRevision;
			} else {
				entry.projection = a_projection;
			}
			++observed.revision;
			return true;
		}

		/** @brief Rechecks projected bindings at a bounded interval to detect changing runtime sources. */
		bool BeginProjectedTextureProbe(std::string_view a_reference, std::string_view a_shape, uint32_t a_frame)
		{
			std::scoped_lock lock(mutex);
			const auto key = Key(a_reference, a_shape);
			const auto found = observed.entries.find(key);
			if (found == observed.entries.end())
				return false;
			if (const auto previous = lastProbedFrame.find(key); previous != lastProbedFrame.end() &&
				a_frame - previous->second < kTextureProbeIntervalFrames)
				return false;
			lastProbedFrame.insert_or_assign(key, a_frame);
			return true;
		}

		/** @brief Copies observations only when a consumer has missed a revision. */
		std::shared_ptr<const Snapshot> ExportIfChanged(uint64_t a_revision) const
		{
			std::scoped_lock lock(mutex);
			return observed.revision == a_revision ? nullptr : std::make_shared<const Snapshot>(observed);
		}

		/** @brief Excludes frame-dynamic projection constants from material registry revisions. */
		std::shared_ptr<const Snapshot> ExportIfIdentityChanged(uint64_t a_identityRevision) const
		{
			std::scoped_lock lock(mutex);
			return observed.identityRevision == a_identityRevision ? nullptr :
				std::make_shared<const Snapshot>(observed);
		}

		void Reset()
		{
			std::scoped_lock lock(mutex);
			observed.entries.clear();
			lastProbedFrame.clear();
			++observed.epoch;
			++observed.revision;
			++observed.identityRevision;
		}

	private:
		mutable std::mutex mutex;
		Snapshot observed;
		std::unordered_map<std::string, uint32_t> lastProbedFrame;
	};
}

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
		struct Snapshot
		{
			uint64_t revision = 0, epoch = 0;
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
			return true;
		}

		/** @brief Copies observations only when a consumer has missed a revision. */
		std::shared_ptr<const Snapshot> ExportIfChanged(uint64_t a_revision) const
		{
			std::scoped_lock lock(mutex);
			return observed.revision == a_revision ? nullptr : std::make_shared<const Snapshot>(observed);
		}

		void Reset()
		{
			std::scoped_lock lock(mutex);
			observed.entries.clear();
			++observed.epoch;
			++observed.revision;
		}

	private:
		mutable std::mutex mutex;
		Snapshot observed;
	};
}

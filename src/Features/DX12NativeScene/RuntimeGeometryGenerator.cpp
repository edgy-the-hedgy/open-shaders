#include "RuntimeGeometryGenerator.h"
#include "Utils/FileDigest.h"

#include <BS_thread_pool.hpp>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESObjectSTAT.h>
#include <Windows.h>
#include <unordered_set>
#include <cmath>
#include <limits>
#include <algorithm>
#include <cctype>
#include <fstream>

namespace DX12Native
{
	namespace
	{
		constexpr uint32_t kMaximumReferences = 2048;
		constexpr uint32_t kMaximumInstances = 8192;
		constexpr uintmax_t kMaximumCatalogBytes = 128ull * 1024 * 1024;

		std::string NormalizeModel(std::string a_path)
		{
			std::ranges::transform(a_path, a_path.begin(), [](unsigned char c) {
				return c == '/' ? '\\' : char(std::tolower(c));
			});
			if (!a_path.starts_with("meshes\\")) a_path = "meshes\\" + a_path;
			return a_path;
		}

		std::array<float, 12> PlacementMatrix(const RE::NiPoint3& a_point,
			const RE::NiPoint3& a_angle, float a_scale)
		{
			const auto sx = std::sin(a_angle.x), cx = std::cos(a_angle.x);
			const auto sy = std::sin(a_angle.y), cy = std::cos(a_angle.y);
			const auto sz = std::sin(a_angle.z), cz = std::cos(a_angle.z);
			// Exact ToRows(PlacementTransform.FromRecord) convention used by
			// offline cell-layout.json (3x4 column-vector transform).
			return {
				cy * cz * a_scale, cy * sz * a_scale, -sy * a_scale, a_point.x,
				(sx * sy * cz - cx * sz) * a_scale,
				(sx * sy * sz + cx * cz) * a_scale, sx * cy * a_scale, a_point.y,
				(cx * sy * cz + sx * sz) * a_scale,
				(cx * sy * sz - sx * cz) * a_scale, cx * cy * a_scale, a_point.z
			};
		}

		std::array<float, 12> Compose(const std::array<float, 12>& a_placement,
			const std::array<float, 12>& a_shape)
		{
			std::array<float, 12> result{};
			for (size_t row = 0; row < 3; ++row) {
				for (size_t column = 0; column < 3; ++column)
					for (size_t axis = 0; axis < 3; ++axis)
						result[row * 4 + column] += a_placement[row * 4 + axis] * a_shape[axis * 4 + column];
				result[row * 4 + 3] = a_placement[row * 4 + 3];
				for (size_t axis = 0; axis < 3; ++axis)
					result[row * 4 + 3] += a_placement[row * 4 + axis] * a_shape[axis * 4 + 3];
			}
			return result;
		}

		std::array<float, 6> Bounds(const std::array<float, 6>& a_local,
			const std::array<float, 12>& a_matrix)
		{
			// Use the representable IEEE-754 infinity directly. The INFINITY macro
			// can expand into overflowing constant arithmetic under MSVC /WX.
			const float infinity = std::numeric_limits<float>::infinity();
			std::array<float, 6> bounds{ infinity, infinity, infinity, -infinity, -infinity, -infinity };
			for (uint32_t corner = 0; corner < 8; ++corner) {
				const float xyz[]{
					a_local[corner & 1 ? 3 : 0], a_local[corner & 2 ? 4 : 1],
					a_local[corner & 4 ? 5 : 2]
				};
				for (uint32_t axis = 0; axis < 3; ++axis) {
					const auto k = axis * 4;
					const auto value = a_matrix[k] * xyz[0] + a_matrix[k + 1] * xyz[1] +
						a_matrix[k + 2] * xyz[2] + a_matrix[k + 3];
					bounds[axis] = std::min(bounds[axis], value);
					bounds[axis + 3] = std::max(bounds[axis + 3], value);
				}
			}
			return bounds;
		}

		bool Valid(const std::array<float, 12>& a_transform)
		{
			if (!std::ranges::all_of(a_transform, [](float f) { return std::isfinite(f); })) return false;
			const double a = a_transform[0], b = a_transform[1], c = a_transform[2];
			const double d = a_transform[4], e = a_transform[5], f = a_transform[6];
			const double g = a_transform[8], h = a_transform[9], i = a_transform[10];
			const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
			return std::isfinite(det) && std::abs(det) > 1e-12;
		}

		bool IsHex(const std::string& a_hash)
		{
			return a_hash.size() == 64 &&
				std::ranges::all_of(a_hash, [](unsigned char c) {
					return c >= '0' && c <= '9' || c >= 'a' && c <= 'f';
				});
		}
	}

	struct RuntimeGeometryGenerator::State
	{
		std::atomic<bool> busy{ false };
		std::atomic<uint64_t> requested{ 0 };
		mutable std::mutex mutex;
		std::string failure;
		uint32_t captured = 0, placed = 0, missing = 0, truncated = 0;
		uint64_t publications = 0;
	};

	RuntimeGeometryGenerator::RuntimeGeometryGenerator() : state(std::make_shared<State>()) {}

	void RuntimeGeometryGenerator::Reset()
	{
		++generation;
		cellID.store(0, std::memory_order_relaxed);
		lastPollFrame = 0;
		state->requested.store(generation, std::memory_order_release);
	}

	void RuntimeGeometryGenerator::Advance(RE::TESObjectCELL* a_cell, const std::filesystem::path& a_directory,
		const std::string& a_cellKey, uint32_t a_worldspaceID, uint32_t a_frame, uint64_t a_budgetBytes)
	{
		if (!a_cell || !a_cell->IsAttached()) return;
		const auto current = a_cell->GetFormID();
		if (cellID.load(std::memory_order_relaxed) != current) {
			++generation;
			cellID.store(current, std::memory_order_relaxed);
			lastPollFrame = 0;
			state->requested.store(generation, std::memory_order_release);
		}
		if (lastPollFrame && a_frame - lastPollFrame < 180) return;
		lastPollFrame = a_frame;
		if (state->busy.exchange(true, std::memory_order_acq_rel)) return;

		// This is the only engine-facing phase. Capture bounded copies of
		// static reference identity + transform, never RE pointers for workers.
		std::vector<Placement> placements;
		placements.reserve(512);
		uint32_t skipped = 0;
		a_cell->ForEachReference([&](RE::TESObjectREFR* reference) {
			if (placements.size() >= kMaximumReferences) { ++skipped; return RE::BSContainer::ForEachResult::kStop; }
			if (!reference || reference->IsDisabled() || reference->IsMarkedForDeletion()) return RE::BSContainer::ForEachResult::kContinue;
			const auto* base = reference->GetObjectReference();
			if (!base || base->GetFormType() != RE::FormType::Static) return RE::BSContainer::ForEachResult::kContinue;
			const auto* stat = static_cast<const RE::TESObjectSTAT*>(base);
			const auto* model = stat->GetModel();
			if (!model || !*model) return RE::BSContainer::ForEachResult::kContinue;
			const auto position = reference->GetPosition(), rotation = reference->GetAngle();
			const float scale = reference->GetScale();
			if (!std::isfinite(scale) || scale <= 0) return RE::BSContainer::ForEachResult::kContinue;
			auto matrix = PlacementMatrix(position, rotation, scale);
			if (!Valid(matrix)) return RE::BSContainer::ForEachResult::kContinue;
			placements.push_back({ NormalizeModel(model), reference->GetFormID(), base->GetFormID(), matrix });
			return RE::BSContainer::ForEachResult::kContinue;
		});
		if (placements.empty()) {
			std::lock_guard lock(state->mutex);
			state->captured = 0;
			state->truncated = skipped;
			state->busy.store(false, std::memory_order_release);
			return;
		}

		try {
			static BS::thread_pool<> pool{ 1 };
			const auto expected = generation;
			auto owned = state;
			pool.detach_task([owned, expected, directory = a_directory, key = a_cellKey,
				worldspace = a_worldspaceID, placements = std::move(placements), skipped, budget = a_budgetBytes] {
				// Reset busy even on cancellation, cache hit or exceptions.
				const SKSE::stl::scope_exit release([&]() noexcept {
					owned->busy.store(false, std::memory_order_release);
				});
				uint32_t admitted = 0, missing = 0;
				std::string failure;
				try {
					if (owned->requested.load(std::memory_order_acquire) != expected) return;
					// One catalogue read per process/atlas revision, on the
					// dedicated worker. Never read dozens of NIF files here.
					static json catalog;
					static std::filesystem::file_time_type stamp{};
					const auto source = std::filesystem::path(
						"Data/SKSE/Plugins/CommunityShaders/RedstoneScene/atlas/q0/geometry-catalog.json");
					std::error_code error;
					const auto currentStamp = std::filesystem::last_write_time(source, error);
					if (error || std::filesystem::file_size(source) > kMaximumCatalogBytes)
						throw std::runtime_error("Q0 geometry catalogue missing or exceeds 128 MiB");
					if (!catalog.is_object() || currentStamp != stamp) {
						std::ifstream file(source);
						catalog = json::parse(file);
						if (catalog.value("schemaVersion", 0u) != 1 ||
							catalog.value("kind", std::string{}) != "native-runtime-q0-geometry" ||
							!catalog.contains("models") || !catalog["models"].is_object())
							throw std::runtime_error("Unsupported Q0 geometry catalogue schema");
						stamp = currentStamp;
					}
					json instances = json::array();
					std::unordered_set<std::string> uniqueGeometry;
					uint64_t uniqueBytes = 0;
					// Leave headroom for per-resource D3D12 alignment and BLAS
					// allocations. Reject oversized packages instead of letting
					// the Q0 uploader fail and permanently latch a request.
					const uint64_t rawBudget = std::min<uint64_t>(budget / 3, 12ull * 1024 * 1024);
					const auto& models = catalog.at("models");
					for (const auto& placed : placements) {
						const auto found = models.find(placed.model);
						if (found == models.end()) { ++missing; continue; }
						for (const auto& shape : found->at("shapes")) {
							if (instances.size() >= kMaximumInstances) break;
							const auto hash = shape.at("geometrySha256").get<std::string>();
							if (!IsHex(hash) || shape.at("geometryFile").get<std::string>() != "geometry/" + hash + ".rsgm") continue;
							const auto geometryFile = source.parent_path() / "geometry" / (hash + ".rsgm");
							if (!std::filesystem::exists(geometryFile)) continue;
							if (!uniqueGeometry.contains(hash)) {
								if (uniqueGeometry.size() >= 96) continue;
								const auto size = std::filesystem::file_size(geometryFile);
								if (size > rawBudget || uniqueBytes > rawBudget - size) continue;
								uniqueBytes += size;
								uniqueGeometry.insert(hash);
							}
							const auto rows = shape.at("shapeToModel").get<std::array<float, 12>>();
							const auto local = shape.at("localBounds").get<std::array<float, 6>>();
							auto matrix = Compose(placed.transform, rows);
							auto bounds = Bounds(local, matrix);
							if (!Valid(matrix) || !std::ranges::all_of(bounds, [](float f) { return std::isfinite(f); })) continue;
							instances.push_back({
								{ "referenceKey", std::format("{:08X}", placed.referenceID) },
								{ "baseKey", std::format("{:08X}", placed.baseID) },
								{ "model", placed.model },
								{ "geometryFile", shape.at("geometryFile") },
								{ "geometrySha256", hash },
								{ "indexCount", shape.at("indexCount") },
								{ "shapeToCell", matrix },
								{ "cellBounds", bounds },
								{ "opaqueOccluderCandidate", true },
								{ "geometryStatus", "compiled" },
								{ "twoSided", false },
								{ "shadingStatus", "absent:quality" }
							});
							++admitted;
						}
					}
					if (instances.empty()) throw std::runtime_error("No static Q0 meshes matched the installed atlas");
					const auto q0 = directory / "q0";
					std::filesystem::create_directories(q0);
					json layout{
						{ "schemaVersion", 1 }, { "rendererSchemaVersion", 1 },
						{ "shadingSchemaVersion", 2 }, { "materialSchemaVersion", 1 },
						{ "roomSchemaVersion", 1 }, { "rooms", json::array() },
						{ "cell", key }, { "worldspaceFormID", worldspace },
						{ "lights", json::array() }, { "instances", std::move(instances) },
						{ "runtimeCoverageValid", false }, { "generatedFrom", "active-cell-static-atlas-matching" },
						{ "geometryCoverage", "partial-eligible-static-references" }
					};
					const auto temp = q0 / "cell-layout.digest.part";
					{
						std::ofstream file(temp, std::ios::binary | std::ios::trunc);
						file << layout.dump();
						if (!file) throw std::runtime_error("Failed to prepare Q0 geometry digest");
					}
					const auto sha = Util::FileDigest::Sha256FileHex(temp);
					std::filesystem::remove(temp);
					if (!sha) throw std::runtime_error("Q0 geometry digest failed");
					std::string digest = *sha;
					std::ranges::transform(digest, digest.begin(), [](unsigned char c) { return char(std::tolower(c)); });
					layout["layoutDigest"] = digest;
					const auto target = q0 / "cell-layout.json";
					if (std::filesystem::exists(target)) {
						try {
							std::ifstream old(target);
							if (json::parse(old).value("layoutDigest", std::string{}) == digest) {
								std::lock_guard lock(owned->mutex);
								owned->captured = uint32_t(placements.size());
								owned->placed = admitted; owned->missing = missing;
								owned->truncated = skipped;
								return;
							}
						} catch (const std::exception&) { /* Replace a corrupt/incomplete package. */ }
					}
					const auto staged = q0 / "cell-layout.json.part";
					{
						std::ofstream file(staged, std::ios::binary | std::ios::trunc);
						file << layout.dump();
						if (!file) throw std::runtime_error("Failed to stage generated Q0 scene");
					}
					if (owned->requested.load(std::memory_order_acquire) != expected) {
						std::filesystem::remove(staged);
						return;
					}
					if (!MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
						throw std::runtime_error("Failed to atomically publish generated Q0 scene");
					std::lock_guard lock(owned->mutex);
					owned->captured = uint32_t(placements.size());
					owned->placed = admitted; owned->missing = missing;
					owned->truncated = skipped; ++owned->publications;
					owned->failure.clear();
				} catch (const std::exception& e) {
					failure = e.what();
				}
				if (!failure.empty()) {
					std::lock_guard lock(owned->mutex);
					owned->failure = std::move(failure);
					owned->captured = uint32_t(placements.size());
					owned->placed = admitted; owned->missing = missing;
					owned->truncated = skipped;
				}
			});
		} catch (const std::exception& e) {
			std::lock_guard lock(state->mutex);
			state->failure = e.what();
			state->busy.store(false, std::memory_order_release);
		}
	}

	json RuntimeGeometryGenerator::GetDiagnostics() const
	{
		std::lock_guard lock(state->mutex);
		return {
			{ "active", true }, { "busy", state->busy.load(std::memory_order_acquire) },
			{ "cellFormID", cellID.load(std::memory_order_relaxed) }, { "capturedStaticReferences", state->captured },
			{ "generatedInstances", state->placed }, { "uncataloguedReferences", state->missing },
			{ "truncatedReferences", state->truncated }, { "publications", state->publications },
			{ "failure", state->failure }, { "generatorScope", "current-cell-static-geometry-only" }
		};
	}
}

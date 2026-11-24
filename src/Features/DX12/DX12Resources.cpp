#include "DX12Resources.h"

#include "CompanionWorker.h"
#include "DX12Control.h"
#include "Globals.h"
#include "GpuPass.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESWorldSpace.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>
#include <unordered_set>

namespace DX12
{
	constexpr uint64_t kPredictiveTileBudgetBytes = 12ull * 1024 * 1024;

	void ResourceGrid::Advance(uint32_t scope, uint32_t size, double x, double y)
	{
		const auto desired = Neighborhood(scope, size, x, y);
		const auto predictive = PredictiveNeighborhood(scope, size, x, y);
		if (!desired || !predictive)
			return;
		std::scoped_lock lock(mutex);
		if (previousScope == scope && previousSize == size) {
			const double vx = x - previousX, vy = y - previousY;
			const double displacement = vx * vx + vy * vy;
			// Preserve the last non-jitter direction while stationary; disregard teleports.
			if (std::isfinite(displacement) && displacement > 1.0 && displacement < double(size) * size) {
				movementX = vx;
				movementY = vy;
			}
		} else {
			movementX = movementY = 0;
		}
		previousX = x;
		previousY = y;
		previousScope = scope;
		previousSize = size;
		const auto current = snapshot.load(std::memory_order_acquire);
		if (current->requested == *desired && current->size == size)
			return;
		auto next = std::make_shared<Grid>();
		next->revision = current->revision + 1;
		next->geometryRevision = current->geometryRevision + 1;
		next->size = size;
		next->fullNative = current->fullNative;
		if (current->requested.front().scope == scope) next->telemetry = current->telemetry;
		next->requested = *desired;
		next->predictive = *predictive;
		// A failed neighbor may be attempted at most three times while the player
		// remains in the same center subcell. A center transition starts a new
		// bounded retry window for those same neighboring keys.
		const bool playerChangedSubcell = current->requested.front() != next->requested.front();
		if (current->size == size) {
			const auto lookup = [&](SubcellKey key) -> std::shared_ptr<const Tile> {
				for (const auto& old : current->resident)
					if (old && old->key == key)
						return old;
				for (const auto& old : current->predictiveResident)
					if (old && old->key == key)
						return old;
				for (const auto& old : recentTiles)
					if (old && old->key == key)
						return old;
				return {};
			};
			const auto resetFailedAfterMove = [&](std::shared_ptr<const Tile>& tile) {
				if (!playerChangedSubcell || !tile ||
					(tile->state != Tile::State::Incomplete && tile->state != Tile::State::RetryWaiting))
					return;
				auto retry = std::make_shared<Tile>(*tile);
				retry->state = Tile::State::Requested;
				retry->preparationAttempts = 0;
				retry->retryAfter = {};
				retry->failureReason.clear();
				tile = std::move(retry);
			};
			for (size_t i = 0; i < kGridCapacity; ++i) {
				next->resident[i] = lookup(next->requested[i]);
				resetFailedAfterMove(next->resident[i]);
				if (next->resident[i] && next->resident[i]->state == Tile::State::Deferred) {
					// A CPU-budgeted speculative tile is re-requested at full quality on active promotion.
					auto promoted = std::make_shared<Tile>(*next->resident[i]);
					promoted->state = Tile::State::Requested;
					next->resident[i] = std::move(promoted);
				}
			}
			for (size_t i = 0; i < kPredictiveCapacity; ++i) {
				auto tile = lookup(next->predictive[i]);
				resetFailedAfterMove(tile);
				if (tile && (tile->state == Tile::State::Ready || tile->state == Tile::State::ReadyPartial) &&
					tile->bytes > kPredictiveTileBudgetBytes) {
					// Moving from active residency must respect the same 12 MiB per-tile cap
					// as fresh predictive publications. Keep prepared lights available.
					auto deferred = std::make_shared<Tile>(*tile);
					deferred->shapes.clear();
					deferred->bytes = 0;
					deferred->state = Tile::State::Deferred;
					tile = std::move(deferred);
				}
				next->predictiveResident[i] = std::move(tile);
			}
			// Runtime-only geometry follows the active neighborhood. Prune tiles
			// that leave 3x3 without a disk flush or stale GPU ownership.
			for (const auto& observed : current->runtimeShapes)
				if (std::ranges::contains(next->requested, observed.tile)) {
					next->runtimeShapes.push_back(observed);
					next->runtimeBytes += observed.bytes;
				}
		}
		// Keep recently retired immutable cell publications in bounded DRAM.
		// A -> B -> A must not repeat companion processing.
		if (current->size == size) {
			const auto retain = [&](const auto& tile) {
				if (!tile || (tile->state != Tile::State::Ready && tile->state != Tile::State::ReadyPartial))
					return;
				if (std::ranges::contains(next->requested, tile->key) || std::ranges::contains(next->predictive, tile->key))
					return;
				std::erase_if(recentTiles, [&](const auto& cached) { return cached && cached->key == tile->key; });
				recentTiles.push_back(tile);
			};
			for (const auto& tile : current->resident) retain(tile);
			for (const auto& tile : current->predictiveResident) retain(tile);
			while (recentTiles.size() > 32) recentTiles.pop_front();
		}
		if (pending && std::ranges::find(next->requested, *pending) == next->requested.end() &&
			std::ranges::find(next->predictive, *pending) == next->predictive.end())
			pending.reset();
		snapshot.store(std::move(next), std::memory_order_release);
	}

	std::optional<SubcellKey> ResourceGrid::NextRequest()
	{
		std::scoped_lock lock(mutex);
		if (pending)
			return std::nullopt;
		const auto current = snapshot.load(std::memory_order_acquire);
		const auto center = current->requested.front();
		if (!center.scope)
			return std::nullopt;

		const auto start = [&](SubcellKey key, bool speculative, size_t index) -> std::optional<SubcellKey> {
			const auto& existing = speculative ? current->predictiveResident[index] : current->resident[index];
			if (!key.scope || (existing && existing->state != Tile::State::Requested &&
								  (existing->state != Tile::State::RetryWaiting || std::chrono::steady_clock::now() < existing->retryAfter)))
				return std::nullopt;
			auto tile = existing ? std::make_shared<Tile>(*existing) : std::make_shared<Tile>();
			tile->key = key;
			tile->state = Tile::State::Preparing;
			auto next = std::make_shared<Grid>(*current);
			++next->revision;
			if (speculative)
				next->predictiveResident[index] = std::move(tile);
			else
				next->resident[index] = std::move(tile);
			snapshot.store(std::move(next), std::memory_order_release);
			pending = key;
			return pending;
		};
		const auto sorted = [&](auto keys, size_t begin, size_t end) {
			std::stable_sort(keys.begin() + begin, keys.begin() + end, [&](size_t a, size_t b) {
				const auto& left = current->requested[a];
				const auto& right = current->requested[b];
				return (left.x - center.x) * movementX + (left.y - center.y) * movementY >
				       (right.x - center.x) * movementX + (right.y - center.y) * movementY;
			});
			return keys;
		};
		std::array<size_t, kGridCapacity> activeOrder{};
		for (size_t i = 0; i < activeOrder.size(); ++i)
			activeOrder[i] = i;
		activeOrder = sorted(activeOrder, 1, activeOrder.size());
		for (size_t i : activeOrder) {
			const auto& key = current->requested[i];
			if (const auto request = start(key, false, i))
				return request;
		}
		std::array<size_t, kPredictiveCapacity> order{};
		for (size_t i = 0; i < order.size(); ++i)
			order[i] = i;
		std::stable_sort(order.begin(), order.begin() + 12, [&](size_t a, size_t b) {
			const auto& left = current->predictive[a];
			const auto& right = current->predictive[b];
			return (left.x - center.x) * movementX + (left.y - center.y) * movementY >
			       (right.x - center.x) * movementX + (right.y - center.y) * movementY;
		});
		std::stable_sort(order.begin() + 12, order.end(), [&](size_t a, size_t b) {
			const auto& left = current->predictive[a];
			const auto& right = current->predictive[b];
			return (left.x - center.x) * movementX + (left.y - center.y) * movementY >
			       (right.x - center.x) * movementX + (right.y - center.y) * movementY;
		});
		for (size_t i : order) {
			const auto& key = current->predictive[i];
			if (const auto request = start(key, true, i))
				return request;
		}
		return std::nullopt;
	}

	bool ResourceGrid::Publish(CompanionPublication publication)
	{
		std::scoped_lock lock(mutex);
		if (!pending || *pending != publication.key)
			return false;
		pending.reset();
		const auto current = snapshot.load(std::memory_order_acquire);
		const auto active = std::ranges::find(current->requested, publication.key);
		const auto predictive = std::ranges::find(current->predictive, publication.key);
		if (active == current->requested.end() && predictive == current->predictive.end())
			return false;
		auto tile = std::make_shared<Tile>();
		tile->key = publication.key;
		// Retain bounded retry state across failed publications of the same tile.
		const auto existing = active != current->requested.end() ?
		                          current->resident[std::distance(current->requested.begin(), active)] :
		                          current->predictiveResident[std::distance(current->predictive.begin(), predictive)];
		tile->preparationAttempts = static_cast<uint8_t>(std::min<unsigned>(existing ? unsigned(existing->preparationAttempts) + 1 : 1u, 3u));
		tile->failureReason = std::move(publication.failureReason);
		tile->rejectionReasons = std::move(publication.rejectionReasons);
		tile->nifSources = std::move(publication.nifSources);
		tile->missingShapes = std::move(publication.missingShapes);
		// The worker has already verified all positions, triangle indices, transforms, identities and budgets.
		// Render-thread publication only transfers ownership of immutable GPU-scene inputs.
		tile->shapes = std::move(publication.shapes);
		tile->bytes = publication.geometryBytes;
		tile->lights = std::move(publication.lights);
		const bool complete = publication.validated && publication.enumerationComplete &&
		                      publication.failedShapes == 0 && publication.rejectedLights == 0 &&
		                      publication.eligibleShapes == tile->shapes.size();
		if (publication.validated && (complete || !tile->shapes.empty() || !tile->lights.empty())) {
			// Geometry validity and complete static-shadow coverage are different contracts.
			// Keep real partial geometry and placed lights available for native scene building.
			// Empty incomplete results must not be mislabeled as prepared subcells.
			// Only Ready (certified complete) subcells can offload raster shadows.
			tile->state = complete ? Tile::State::Ready : Tile::State::ReadyPartial;
		} else if (publication.fullNative && publication.validated && publication.enumerationComplete) {
			// An interior-wide unresolved record can make an otherwise empty tile
			// uncertifiable. Repeating the same valid enumeration will not fix it.
			tile->state = Tile::State::EmptyUncertified;
		} else {
			// Malformed or empty-incomplete companion payloads must not claim native scene readiness.
			tile->shapes.clear();
			tile->lights.clear();
			tile->bytes = 0;
			if (tile->preparationAttempts < 3) {
				tile->state = Tile::State::RetryWaiting;
				tile->retryAfter = std::chrono::steady_clock::now() +
				                   std::chrono::milliseconds(tile->preparationAttempts == 1 ? 500 : 2000);
			} else {
				tile->state = Tile::State::Incomplete;
			}
		}
		auto next = std::make_shared<Grid>(*current);
		++next->revision;
		if (publication.validated) {
			next->fullNative = publication.fullNative;
			if (publication.telemetry) next->telemetry = std::move(publication.telemetry);
		}
		if (active != current->requested.end()) {
			++next->geometryRevision;
			next->resident[std::distance(current->requested.begin(), active)] = std::move(tile);
		} else {
			// Sixteen predictive tiles have a strict combined 192 MiB CPU geometry allowance
			// (12 MiB per tile), independent of active GPU scene residency.
			if (tile->bytes > kPredictiveTileBudgetBytes) {
				tile->shapes.clear();
				tile->bytes = 0;
				tile->state = Tile::State::Deferred;
			}
			// A ready predictive emitter can cast toward visible receivers even when
			// the emitter is off-screen. Update lights, never BLAS/TLAS for this tile.
			next->predictiveResident[std::distance(current->predictive.begin(), predictive)] = std::move(tile);
		}
		snapshot.store(std::move(next), std::memory_order_release);
		return true;
	}

	bool ResourceGrid::RecoverMaterial(uint32_t scope, const std::shared_ptr<const Shape>& matched,
		const std::shared_ptr<const PreparedShading>& shading)
	{
		if (!matched || !shading || !shading->runtimeObserved || !matched->identity ||
			shading->stride != 60 || shading->attributes.size() != matched->Positions().size() * 15u)
			return false;
		std::scoped_lock lock(mutex);
		const auto current = snapshot.load(std::memory_order_acquire);
		if (!current->fullNative || current->requested.front().scope != scope)
			return false;
		const uint64_t extra = uint64_t(shading->attributes.size()) * sizeof(float);
		// Charge replacement conservatively without freeing the old attributes
		// until all prior immutable generations retire. Do not grow predictive tiles.
		constexpr uint64_t maxTileBytes = 112ull * 1024 * 1024;
		auto next = std::make_shared<Grid>(*current);
		std::shared_ptr<const Shape> replacement;
		for (auto& tile : next->resident) {
			if (!tile || tile->bytes > maxTileBytes || extra > maxTileBytes - tile->bytes) continue;
			std::shared_ptr<Tile> changed;
			for (size_t i = 0; i < tile->shapes.size(); ++i) {
				const auto& original = tile->shapes[i];
				if (!original || original->identity != matched->identity ||
					original->referencePlugin != matched->referencePlugin ||
					original->referenceLocal != matched->referenceLocal ||
					original->GeometryKey() != matched->GeometryKey() || original->transform != matched->transform ||
					(original->shading && !(original->materialOverride && original->materialOverride->ambiguous)))
					continue;
				if (!replacement) {
					auto copy = std::make_shared<Shape>(*original);
					auto live = std::make_shared<PreparedShading>(*shading);
					live->recoveredPlacement = true;
					copy->shading = std::move(live);
					// The observed engine texture set is the winning binding. Do
					// not reapply the unresolved offline alternate to it.
					copy->materialOverride.reset();
					replacement = std::move(copy);
				}
				if (!changed) changed = std::make_shared<Tile>(*tile);
				changed->shapes[i] = replacement;
				changed->bytes += extra;
				break;
			}
			if (changed) tile = std::move(changed);
		}
		if (!replacement) return false;
		++next->revision;
		++next->geometryRevision;
		snapshot.store(std::move(next), std::memory_order_release);
		return true;
	}

	bool ResourceGrid::PublishRuntimeShape(std::shared_ptr<const Shape> shape, float centerX, float centerY)
	{
		// Larger interior furniture and room pieces often exceed 256 KiB.
		// Keep the whole runtime supplement transient and strictly budgeted.
		constexpr size_t kMaximumRuntimeShapes = 2048;
		constexpr uint64_t kMaximumRuntimeBytes = 64ull * 1024 * 1024;
		constexpr uint64_t kMaximumSingleShapeBytes = 1024ull * 1024;
		if (!shape || !shape->identity || shape->Positions().empty() || shape->Indices().empty() ||
			shape->Indices().size() % 3)
			return false;
		const uint64_t shapeBytes = uint64_t(shape->Positions().size()) * sizeof(float3) +
		                            uint64_t(shape->Indices().size()) * sizeof(uint32_t);
		if (!shapeBytes || shapeBytes > kMaximumSingleShapeBytes)
			return false;
		const uint64_t shadingBytes = shape->shading ? uint64_t(shape->shading->attributes.size()) * sizeof(float) : 0;
		const uint64_t residentBytes = shapeBytes + shadingBytes;
		std::scoped_lock lock(mutex);
		const auto current = snapshot.load(std::memory_order_acquire);
		if (!current->requested.front().scope)
			return false;
		const auto x = Coordinate(centerX, current->size);
		const auto y = Coordinate(centerY, current->size);
		if (!x || !y)
			return false;
		const SubcellKey key{ current->requested.front().scope, *x, *y };
		if (!std::ranges::contains(current->requested, key))
			return false;
		if (std::ranges::any_of(current->runtimeShapes, [&](const RuntimeShape& item) {
				return item.shape && item.shape->identity == shape->identity;
			}))
			return false;
		auto next = std::make_shared<Grid>(*current);
		if (shape->runtimeObjectKey) {
			// A new transform replaces the prior observation of this physical mesh.
			// Existing immutable GPU snapshots keep old geometry alive until retirement.
			std::erase_if(next->runtimeShapes, [&](const RuntimeShape& observed) {
				if (!observed.shape || observed.shape->runtimeObjectKey != shape->runtimeObjectKey)
					return false;
				next->runtimeBytes -= observed.bytes;
				return true;
			});
		}
		if (next->runtimeShapes.size() >= kMaximumRuntimeShapes ||
			residentBytes > kMaximumRuntimeBytes - next->runtimeBytes)
			return false;
		next->runtimeShapes.push_back({ key, std::move(shape), residentBytes });
		next->runtimeBytes += residentBytes;
		++next->revision;
		++next->geometryRevision;
		snapshot.store(std::move(next), std::memory_order_release);
		return true;
	}

	void ResourceGrid::FlushRetiredCache()
	{
		std::scoped_lock lock(mutex);
		recentTiles.clear();
	}

	void ResourceGrid::Clear()
	{
		std::scoped_lock lock(mutex);
		pending.reset();
		recentTiles.clear();
		previousScope = previousSize = 0;
		movementX = movementY = 0;
		const auto current = snapshot.load(std::memory_order_acquire);
		auto next = std::make_shared<Grid>();
		next->revision = current->revision + 1;
		next->geometryRevision = current->geometryRevision + 1;
		snapshot.store(std::move(next), std::memory_order_release);
	}

	void ResourceGrid::CancelPending()
	{
		std::scoped_lock lock(mutex);
		if (!pending)
			return;
		const auto current = snapshot.load(std::memory_order_acquire);
		auto next = std::make_shared<Grid>(*current);
		const auto reset = [&](auto& keys, auto& resident) {
			const auto found = std::ranges::find(keys, *pending);
			if (found == keys.end())
				return;
			const auto i = size_t(std::distance(keys.begin(), found));
			if (resident[i] && resident[i]->state == Tile::State::Preparing) {
				auto tile = std::make_shared<Tile>(*resident[i]);
				tile->state = Tile::State::Requested;
				resident[i] = std::move(tile);
			}
		};
		reset(next->requested, next->resident);
		reset(next->predictive, next->predictiveResident);
		++next->revision;
		pending.reset();
		snapshot.store(std::move(next), std::memory_order_release);
	}

}

namespace
{
	const char* TileStatus(const std::shared_ptr<const DX12::Tile>& tile)
	{
		if (!tile)
			return T("menu.dx12.resources.status.not_processed", "Not processed");
		switch (tile->state) {
		case DX12::Tile::State::Ready:
			return T("menu.dx12.resources.status.loaded", "Loaded");
		case DX12::Tile::State::ReadyPartial:
			return T("menu.dx12.resources.status.partially_loaded", "Partially loaded");
		case DX12::Tile::State::Preparing:
			return T("menu.dx12.resources.status.processing", "Processing");
		case DX12::Tile::State::RetryWaiting:
			return T("menu.dx12.resources.status.retry_pending", "Failed (retry pending)");
		case DX12::Tile::State::EmptyUncertified:
			return "Empty (coverage unverified)";
		case DX12::Tile::State::Incomplete:
			return T("menu.dx12.resources.status.retry_exhausted", "Skipped (retry exhausted)");
		case DX12::Tile::State::Deferred:
			return T("menu.dx12.resources.status.predictive_budget", "Skipped (predictive budget)");
		case DX12::Tile::State::Requested:
			return T("menu.dx12.resources.status.not_processed", "Not processed");
		}
		return T("menu.dx12.resources.status.unknown", "Unknown");
	}

	ImU32 TileColor(const std::shared_ptr<const DX12::Tile>& tile, bool active)
	{
		if (!tile)
			return IM_COL32(46, 54, 65, 255);  // Awaiting generation: dark slate.
		switch (tile->state) {
		case DX12::Tile::State::Ready:
			return active ? IM_COL32(243, 201, 74, 255) : IM_COL32(117, 197, 239, 255);
		case DX12::Tile::State::ReadyPartial:
			return active ? IM_COL32(200, 164, 71, 255) : IM_COL32(91, 156, 194, 255);
		case DX12::Tile::State::Preparing:
			return IM_COL32(182, 140, 91, 255);
		case DX12::Tile::State::RetryWaiting:
			return IM_COL32(211, 76, 76, 255);
		case DX12::Tile::State::EmptyUncertified:
		case DX12::Tile::State::Incomplete:
			return IM_COL32(123, 129, 139, 255);
		case DX12::Tile::State::Deferred:
			return IM_COL32(123, 129, 139, 255);
		case DX12::Tile::State::Requested:
			return IM_COL32(46, 54, 65, 255);
		}
		return IM_COL32(46, 54, 65, 255);
	}

	std::shared_ptr<const DX12::Tile> FindTile(
		const DX12::Grid& grid, DX12::SubcellKey key, bool active)
	{
		if (active) {
			for (size_t i = 0; i < DX12::kGridCapacity; ++i)
				if (grid.requested[i] == key)
					return grid.resident[i];
		} else {
			for (size_t i = 0; i < DX12::kPredictiveCapacity; ++i)
				if (grid.predictive[i] == key)
					return grid.predictiveResident[i];
		}
		return {};
	}

	void DrawSubcellMap(const DX12::Grid& grid, bool diagnostics = false)
	{
		const auto center = grid.requested.front();
		const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
		const float pitch = std::min(54.0f, std::max(28.0f, (available - 4.0f) / 5.0f));
		const float side = pitch - 4.0f;
		const float total = pitch * 5.0f;
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float left = origin.x + std::max(0.0f, (available - total) * 0.5f);
		ImDrawList* draw = ImGui::GetWindowDrawList();
		for (int row = 0; row < 5; ++row) {
			for (int col = 0; col < 5; ++col) {
				const int dx = col - 2, dy = 2 - row;
				const bool active = std::max(std::abs(dx), std::abs(dy)) <= 1;
				const DX12::SubcellKey key{ center.scope, center.x + dx, center.y + dy };
				const auto tile = center.scope ? FindTile(grid, key, active) : nullptr;
				const ImVec2 p0{ left + col * pitch, origin.y + row * pitch };
				const ImVec2 p1{ p0.x + side, p0.y + side };
				draw->AddRectFilled(p0, p1, TileColor(tile, active), 3.0f);
				draw->AddRect(p0, p1, active ? IM_COL32(255, 225, 150, 200) : IM_COL32(138, 174, 207, 180), 3.0f, 0, active ? 1.8f : 1.0f);
				if (tile && tile->state == DX12::Tile::State::ReadyPartial) {
					// A diagonal distinguishes partially processed cells without hiding the zone color.
					draw->AddLine(ImVec2(p0.x + 4.0f, p1.y - 4.0f),
						ImVec2(p1.x - 4.0f, p0.y + 4.0f), IM_COL32(32, 44, 56, 170), 2.0f);
				}
				if (col == 2 && row == 2) {
					const ImVec2 marker{ (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f };
					draw->AddCircleFilled(marker, 5.0f, IM_COL32(30, 37, 48, 255));
					draw->AddCircle(marker, 5.0f, IM_COL32(255, 255, 255, 240), 0, 1.5f);
				}
			}
		}
		ImGui::Dummy(ImVec2(available, total));
		// Hover uses the immutable grid snapshot, so no worker or scene locks are taken.
		if (center.scope && ImGui::IsItemHovered()) {
			const ImVec2 mouse = ImGui::GetIO().MousePos;
			const int col = int((mouse.x - left) / pitch);
			const int row = int((mouse.y - origin.y) / pitch);
			if (mouse.x >= left && mouse.y >= origin.y && col >= 0 && col < 5 && row >= 0 && row < 5) {
				const int dx = col - 2, dy = 2 - row;
				const bool active = std::max(std::abs(dx), std::abs(dy)) <= 1;
				const DX12::SubcellKey key{ center.scope, center.x + dx, center.y + dy };
				const auto tile = FindTile(grid, key, active);
				ImGui::BeginTooltip();
				ImGui::Text(T("menu.dx12.resources.tooltip.subcell", "%s subcell (%lld, %lld): %s"), active ? T("menu.dx12.resources.active", "Active") : T("menu.dx12.resources.predictive", "Predictive"),
					static_cast<long long>(key.x), static_cast<long long>(key.y), TileStatus(tile));
				if (diagnostics && tile && tile->preparationAttempts)
					ImGui::Text(T("menu.dx12.resources.tooltip.attempts", "Attempts: %u / 3"), unsigned(tile->preparationAttempts));
				if (diagnostics && tile && (!tile->nifSources.empty() || !tile->missingShapes.empty()))
					ImGui::Text(T("menu.dx12.resources.tooltip.nif_sources", "NIF sources: %zu | Missing shape references: %zu"),
						tile->nifSources.size(), tile->missingShapes.size());
				if (diagnostics && tile && !tile->failureReason.empty())
					ImGui::TextWrapped(T("menu.dx12.resources.tooltip.last_reason", "Last reason: %s"), tile->failureReason.c_str());
				if (diagnostics && tile)
					for (const auto& [reason, count] : tile->rejectionReasons)
						ImGui::Text("%s: %u", reason.c_str(), count);
				ImGui::EndTooltip();
			}
		}
	}
}

bool DX12Resources::IsActive() const
{
	return loaded && globals::features::dx12Control.IsActive() && !globals::game::isVR;
}

DX12Resources::DX12Resources() = default;
DX12Resources::~DX12Resources() = default;

void DX12Resources::ResetPreparation()
{
	// A new subcell size or world-scene epoch invalidates the grid and all
	// companion work. Keep this off the normal settings-load path when unchanged.
	if (companion)
		companion->Cancel();
	inFlight.reset();
	adopting.reset();
	lightAdoptionOffset = 0;
	grid.Clear();
	lastVisitedCell = previousVisitedCell = distinctTransitions = 0;
	flushGeneration.fetch_add(1, std::memory_order_release);
	generationStats.store(std::make_shared<const DX12::PreparationStats>(), std::memory_order_release);
}

void DX12Resources::DrawSettings()
{
	int index = int(std::ranges::find(DX12::kSubcellSizes, settings.SubcellSize) - DX12::kSubcellSizes.begin());
	if (index >= int(DX12::kSubcellSizes.size()))
		index = 0;
	constexpr const char* labels[] = { "4096", "6144", "8192", "10240", "12288", "14336", "16384" };
	if (ImGui::Combo(T("menu.dx12.resources.subcell_size", "Subcell size (game units)"), &index, labels, int(std::size(labels)))) {
		const uint32_t selected = DX12::kSubcellSizes[index];
		if (settings.SubcellSize != selected) {
			settings.SubcellSize = selected;
			ResetPreparation();
		}
	}

	ImGui::SeparatorText(T("menu.dx12.resources.subcell_status", "Subcell status (5x5)"));
	const auto snapshot = GetGrid();
	if (snapshot)
		DrawSubcellMap(*snapshot);
	ImGui::TextColored(ImVec4(0.95f, 0.79f, 0.29f, 1.0f), T("menu.dx12.resources.legend.yellow", "Yellow: loaded 3x3"));
	ImGui::TextColored(ImVec4(0.46f, 0.77f, 0.90f, 1.0f), T("menu.dx12.resources.legend.light_blue", "Light blue: loaded predictive ring"));
	ImGui::TextColored(ImVec4(0.83f, 0.30f, 0.30f, 1.0f), T("menu.dx12.resources.legend.red", "Red: failed (retry pending)"));
	ImGui::TextDisabled(T("menu.dx12.resources.legend.diagonal", "Diagonal: partially loaded  |  Center dot: player subcell"));
}

void DX12Resources::DrawDevStats()
{
	ImGui::SeparatorText(T("menu.dx12.resources.subcell_status", "Subcell status (5x5)"));
	const auto snapshot = GetGrid();
	if (snapshot) {
		DrawSubcellMap(*snapshot, true);
		size_t activeLoaded = 0, predictiveLoaded = 0, placements = 0, shaded = 0, missing = 0;
		std::array<size_t, 8> activeStates{}, predictiveStates{};
		uint64_t chargedBytes = 0, uniqueGeometryBytes = 0, uniqueShadingBytes = 0;
		std::unordered_set<const void*> uniqueGeometry;
		std::unordered_set<const DX12::PreparedShading*> uniqueShading;
		std::unordered_set<std::string> uniqueMissing, missingReferences;
		std::unordered_map<std::string, size_t> missingReasons;
		const auto visit = [&](const auto& tile, bool active) {
			if (!tile) { ++(active ? activeStates : predictiveStates)[0]; return; }
			++(active ? activeStates : predictiveStates)[size_t(tile->state)];
			if (tile->state == DX12::Tile::State::Ready || tile->state == DX12::Tile::State::ReadyPartial)
				(active ? activeLoaded : predictiveLoaded)++;
			chargedBytes += tile->bytes;
			missing += tile->missingShapes.size();
			for (const auto& item : tile->missingShapes) {
				const auto reference = std::format("{}:{:06X}", item.referencePlugin, item.referenceLocal);
				missingReferences.insert(reference);
				if (uniqueMissing.insert(std::format("{}:{}:{}", reference, item.nifPath, item.blockIndex)).second)
					++missingReasons[item.reason];
			}
			for (const auto& shape : tile->shapes) {
				if (!shape) continue;
				++placements;
				if (uniqueGeometry.insert(shape->GeometryKey()).second)
					uniqueGeometryBytes += uint64_t(shape->Positions().size()) * sizeof(float3) + uint64_t(shape->Indices().size()) * sizeof(uint32_t);
				if (shape->shading) {
					++shaded;
					if (uniqueShading.insert(shape->shading.get()).second)
						uniqueShadingBytes += shape->shading->attributes.size() * sizeof(float);
				}
			}
		};
		for (const auto& tile : snapshot->resident) visit(tile, true);
		for (const auto& tile : snapshot->predictiveResident) visit(tile, false);
		ImGui::Text(T("menu.dx12.resources.loaded_counts", "Loaded: active %zu / 9, predictive %zu / 16"), activeLoaded, predictiveLoaded);
		ImGui::Text("Active / predictive: empty unverified %zu / %zu | exhausted %zu / %zu | budget deferred %zu / %zu",
			activeStates[size_t(DX12::Tile::State::EmptyUncertified)], predictiveStates[size_t(DX12::Tile::State::EmptyUncertified)],
			activeStates[size_t(DX12::Tile::State::Incomplete)], predictiveStates[size_t(DX12::Tile::State::Incomplete)],
			activeStates[size_t(DX12::Tile::State::Deferred)], predictiveStates[size_t(DX12::Tile::State::Deferred)]);
		ImGui::Text("Placements: %zu | Unique geometry: %zu | Shaded: %zu | Missing records: %zu", placements, uniqueGeometry.size(), shaded, missing);
		ImGui::Text("CPU geometry: %.2f MiB | Shading: %.2f MiB | Charged: %.2f MiB", double(uniqueGeometryBytes)/1048576.0, double(uniqueShadingBytes)/1048576.0, double(chargedBytes)/1048576.0);
		ImGui::Text("Unique missing descriptors: %zu | References: %zu", uniqueMissing.size(), missingReferences.size());
		if (ImGui::TreeNode("Missing descriptor reasons (deduplicated)")) {
			std::vector<std::pair<std::string, size_t>> reasons(missingReasons.begin(), missingReasons.end());
			std::ranges::sort(reasons, [](const auto& a, const auto& b) { return a.second > b.second; });
			for (const auto& [reason, count] : reasons) ImGui::Text("%s: %zu", reason.c_str(), count);
			ImGui::TreePop();
		}
		size_t shadedRuntime = 0;
		for (const auto& observed : snapshot->runtimeShapes)
			if (observed.shape && observed.shape->shading) ++shadedRuntime;
		ImGui::Text("Runtime supplements: %zu | Shaded: %zu | %.2f MiB", snapshot->runtimeShapes.size(), shadedRuntime, double(snapshot->runtimeBytes)/1048576.0);
		if (const auto& telemetry = snapshot->telemetry; snapshot->fullNative && telemetry) {
			ImGui::SeparatorText("Full Native deployment and cache");
			ImGui::Text("Mod manager: %s | Profile: %s | PG entries: %u", telemetry->manager.c_str(), telemetry->profile.empty() ? "--" : telemetry->profile.c_str(), telemetry->pgEntries);
			if (telemetry->exteriorTotal)
				ImGui::Text("Exterior bounds: %u / %u | %s", telemetry->exteriorIndexed, telemetry->exteriorTotal,
					!telemetry->exteriorFinished ? "indexing" : telemetry->exteriorIncomplete ? "unsupported sources remain" : "indexed");
			ImGui::Text("Cache: %.2f MiB | Entries: %u", double(telemetry->cacheBytes)/1048576.0, telemetry->cacheEntries);
			ImGui::Text("Session cache hits: %llu | Misses: %llu | Evictions: %llu",
				static_cast<unsigned long long>(telemetry->cacheHits), static_cast<unsigned long long>(telemetry->cacheMisses), static_cast<unsigned long long>(telemetry->cacheEvictions));
		}
	}
	ImGui::SeparatorText(T("menu.dx12.resources.stats.title", "Subcell generation stats"));
	ImGui::TextDisabled(T("menu.dx12.resources.stats.timing_head", "Last / Avg / Worst (milliseconds)"));
	const auto timing = GetPreparationStats();
	const auto show = [](const char* label, const DX12::PreparationTiming& value) {
		if (value.completed)
			ImGui::Text("%s: %.1f / %.1f / %.1f ms", label, value.lastMs, value.AverageMs(), value.worstMs);
		else
			ImGui::TextDisabled("%s: -- / -- / --", label);
	};
	if (timing) {
		show(T("menu.dx12.resources.stats.main", "Main"), timing->active);
		show(T("menu.dx12.resources.predictive", "Predictive"), timing->predictive);
		show(T("menu.dx12.resources.stats.corners", "Corners"), timing->corners);
	}
}

void DX12Resources::LoadSettings(json& input)
{
	const uint32_t requested = input.value("SubcellSize", DX12::kSubcellSizes.front());
	const uint32_t selected = std::ranges::contains(DX12::kSubcellSizes, requested) ?
	                              requested :
	                              DX12::kSubcellSizes.front();
	if (settings.SubcellSize != selected) {
		settings.SubcellSize = selected;
		ResetPreparation();
	}
}

void DX12Resources::SaveSettings(json& output)
{
	output = { { "SubcellSize", settings.SubcellSize } };
}

void DX12Resources::RestoreDefaultSettings()
{
	const uint32_t defaultSize = DX12::kSubcellSizes.front();
	if (settings.SubcellSize != defaultSize) {
		settings.SubcellSize = defaultSize;
		ResetPreparation();
	}
}

void DX12Resources::PreShadowMaps()
{
	CS_GPU_PASS("DX12Resources::Grid");
	if (!IsActive()) {
		if (grid.Snapshot()->requested.front().scope || inFlight || adopting)
			ResetPreparation();
		return;
	}
	const bool selectedProfile = globals::features::dx12Control.IsFullNativeMode();
	if (selectedProfile != fullNativeProfile) {
		ResetPreparation();
		fullNativeProfile = selectedProfile;
	}
	const auto* player = globals::game::player;
	const auto* cell = player ? player->GetParentCell() : nullptr;
	if (!cell)
		return;
	const auto* world = cell->IsExteriorCell() ? player->GetWorldspace() : nullptr;
	const uint32_t cellID = cell->GetFormID();
	if (lastVisitedCell && cellID && cellID != lastVisitedCell) {
		if (cellID == previousVisitedCell)
			distinctTransitions = 0;  // Returning to a visited cell.
		else if (++distinctTransitions >= 2) {
			grid.FlushRetiredCache();
			flushGeneration.fetch_add(1, std::memory_order_release);
			distinctTransitions = 0;
		}
		previousVisitedCell = lastVisitedCell;
	}
	if (cellID)
		lastVisitedCell = cellID;
	const uint32_t scope = world ? world->GetFormID() : cell->GetFormID();
	const auto position = player->GetPosition();
	grid.Advance(scope, settings.SubcellSize, position.x, position.y);
	if (!companion)
		companion = std::make_unique<DX12::CompanionWorker>();
	const auto neighborhood = grid.Snapshot();
	if (inFlight) {
		const bool stillWanted = std::ranges::contains(neighborhood->requested, *inFlight) ||
		                         std::ranges::contains(neighborhood->predictive, *inFlight);
		// An in-flight predictive request must yield when an active tile becomes newly necessary.
		const bool speculative = !std::ranges::contains(neighborhood->requested, *inFlight);
		const bool activeMissing = std::ranges::any_of(neighborhood->resident, [](const auto& tile) {
			return !tile || tile->state == DX12::Tile::State::Requested;
		});
		if (!stillWanted || (speculative && activeMissing)) {
			companion->Cancel();
			grid.CancelPending();
			inFlight.reset();
		}
	}
	if (auto result = companion->Take()) {
		inFlight.reset();
		adopting = std::make_unique<DX12::CompanionResult>(std::move(*result));
		adopting->publication.failureReason = adopting->failure.substr(0, 320);
		adopting->publication.lights.reserve(adopting->lights.size());
		lightAdoptionOffset = 0;
	}
	if (adopting) {
		const auto key = adopting->publication.key;
		const bool active = std::ranges::contains(neighborhood->requested, key);
		const bool predictive = std::ranges::contains(neighborhood->predictive, key);
		const bool activeMissing = std::ranges::any_of(neighborhood->resident, [](const auto& tile) {
			return !tile || tile->state == DX12::Tile::State::Requested;
		});
		if ((!active && !predictive) || (predictive && activeMissing)) {
			// A superseded speculative publication must not block an active tile.
			adopting.reset();
			lightAdoptionOffset = 0;
			grid.CancelPending();
		} else {
			// Only engine FormID lookup must run on the game thread. Keep it bounded
			// while immutable geometry remains off-thread prepared.
			constexpr size_t kFormResolutionsPerFrame = 64;
			const size_t end = std::min(adopting->lights.size(), lightAdoptionOffset + kFormResolutionsPerFrame);
			auto* handler = RE::TESDataHandler::GetSingleton();
			for (; lightAdoptionOffset < end; ++lightAdoptionOffset) {
				auto& record = adopting->lights[lightAdoptionOffset];
				const auto* reference = handler ? handler->LookupForm(record.referenceLocal, record.referencePlugin) : nullptr;
				const auto* base = handler ? handler->LookupForm(record.baseLocal, record.basePlugin) : nullptr;
				if (!reference || !base) {
					// This light cannot be matched; it must not invalidate unrelated
					// geometry or shadow coverage for other successfully resolved lights.
					continue;
				}
				record.value.referenceID = reference->GetFormID();
				record.value.baseID = base->GetFormID();
				adopting->publication.lights.push_back(record.value);
			}
			if (lightAdoptionOffset == adopting->lights.size()) {
				const bool wasPredictiveRequest = adopting->predictive;
				const bool wasCornerRequest = adopting->corner;
				const double elapsedMs = adopting->preparationMs;
				if (grid.Publish(std::move(adopting->publication))) {
					// This snapshot only counts accepted, finished worker requests; canceled
					// and superseded work is not misreported as generated geometry.
					auto next = std::make_shared<DX12::PreparationStats>(
						*generationStats.load(std::memory_order_acquire));
					auto& timing = !wasPredictiveRequest ? next->active :
					               wasCornerRequest      ? next->corners :
					                                       next->predictive;
					++timing.completed;
					timing.lastMs = elapsedMs;
					timing.totalMs += elapsedMs;
					timing.worstMs = std::max(timing.worstMs, elapsedMs);
					generationStats.store(std::move(next), std::memory_order_release);
				}
				adopting.reset();
				lightAdoptionOffset = 0;
			}
		}
	}
	if (!adopting && companion->Idle()) {
		if (const auto request = grid.NextRequest()) {
			const auto* source = world ? static_cast<const RE::TESForm*>(world) : static_cast<const RE::TESForm*>(cell);
			const auto* file = source ? source->GetFile(0) : nullptr;
			if (file && !file->GetFilename().empty()) {
				DX12::CompanionRequest task;
				task.key = *request;
				const auto requestedGrid = grid.Snapshot();
				task.predictive = !std::ranges::contains(requestedGrid->requested, *request);
				// Predictive indices 0..11 are sides; 12..15 are corners.
				// Store the request-time class before a possible player subcell transition.
				task.corner = task.predictive &&
				              std::ranges::find(requestedGrid->predictive.begin() + 12,
								  requestedGrid->predictive.end(), *request) != requestedGrid->predictive.end();
				task.size = settings.SubcellSize;
				task.localScopeID = source->GetLocalFormID();
				task.scopePlugin = file->GetFilename();
				task.worldspace = world != nullptr;
				task.fullNative = fullNativeProfile;
				if (task.fullNative) {
					if (auto* handler = RE::TESDataHandler::GetSingleton()) {
						for (uint32_t i = 0; i < handler->GetLoadedModCount(); ++i)
							if (const auto* loadedPlugin = handler->LookupLoadedModByIndex(static_cast<uint8_t>(i)))
								task.loadedPlugins.emplace_back(loadedPlugin->GetFilename());
						for (uint32_t i = 0; i < handler->GetLoadedLightModCount(); ++i)
							if (const auto* loadedLightPlugin = handler->LookupLoadedLightModByIndex(static_cast<uint16_t>(i)))
								task.loadedLightPlugins.emplace_back(loadedLightPlugin->GetFilename());
					}
				}
				if (companion->Enqueue(std::move(task))) {
					inFlight = *request;
					return;
				}
			}
			DX12::CompanionPublication incomplete;
			incomplete.key = *request;
			grid.Publish(std::move(incomplete));
		}
	}
}

void DX12Resources::OnSceneTransitionReset(bool /*opening*/)
{
	// Loading-menu transitions do not flush the DRAM cache.
	// The next observed cell ID determines whether this is a return trip.
}

void DX12Resources::OnRuntimeDisabled()
{
	ResetPreparation();
}

std::vector<DX12::PreparedLight> DX12Resources::GetPreparedLights() const
{
	std::vector<DX12::PreparedLight> lights;
	lights.reserve(DX12::kMaxPreparedLights);
	const auto current = grid.Snapshot();
	std::unordered_set<uint32_t> seen;
	const auto append = [&](const auto& tiles) {
		for (const auto& tile : tiles) {
			if (!tile)
				continue;
			for (const auto& light : tile->lights) {
				// Active tiles come first; never publish more than the shadow consumer can trace.
				if (lights.size() >= DX12::kMaxPreparedLights)
					return;
				if (light.referenceID && !seen.insert(light.referenceID).second)
					continue;
				lights.push_back(light);
			}
		}
	};
	append(current->resident);
	// Screen-space receivers can be shadowed by emitters in the predictive ring.
	// Source visibility inside the camera frustum is not a light-admission test.
	append(current->predictiveResident);
	return lights;
}

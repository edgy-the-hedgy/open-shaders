#include "DX12Resources.h"

#include "CompanionWorker.h"
#include "DX12Control.h"
#include "Globals.h"
#include "GpuPass.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESWorldSpace.h"

#include <algorithm>
#include <cmath>
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

	bool ResourceGrid::PublishRuntimeShape(std::shared_ptr<const Shape> shape, float centerX, float centerY)
	{
		// Larger interior furniture and room pieces often exceed 256 KiB.
		// Keep the whole runtime supplement transient and strictly budgeted.
		constexpr size_t kMaximumRuntimeShapes = 384;
		constexpr uint64_t kMaximumRuntimeBytes = 64ull * 1024 * 1024;
		constexpr uint64_t kMaximumSingleShapeBytes = 1024ull * 1024;
		if (!shape || !shape->identity || shape->positions.empty() || shape->indices.empty() ||
			shape->indices.size() % 3)
			return false;
		const uint64_t shapeBytes = uint64_t(shape->positions.size()) * sizeof(float3) +
		                            uint64_t(shape->indices.size()) * sizeof(uint32_t);
		if (!shapeBytes || shapeBytes > kMaximumSingleShapeBytes)
			return false;
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
			shapeBytes > kMaximumRuntimeBytes - next->runtimeBytes)
			return false;
		next->runtimeShapes.push_back({ key, std::move(shape), shapeBytes });
		next->runtimeBytes += shapeBytes;
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

bool DX12Resources::IsActive() const
{
	return loaded && globals::features::dx12Control.IsActive() && globals::features::dx12Control.IsShadowOffloadMode() && !globals::game::isVR;
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

	const auto snapshot = GetGrid();
	ImGui::TextUnformatted(T("menu.dx12.runtime.source", "Runtime geometry only"));
	ImGui::TextDisabled("%s", T("menu.dx12.runtime.no_prediction", "No companion or unloaded tile preparation"));
	if (snapshot) {
		ImGui::Text(T("menu.dx12.runtime.resident_shapes", "Observed shapes: %zu"), snapshot->runtimeShapes.size());
		ImGui::Text(T("menu.dx12.runtime.resident_mib", "Geometry: %.2f MiB"), double(snapshot->runtimeBytes) / (1024.0 * 1024.0));
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
	if (!IsActive()) {
		if (grid.Snapshot()->requested.front().scope || inFlight || adopting)
			ResetPreparation();
		return;
	}
	const auto* player = globals::game::player;
	const auto* cell = player ? player->GetParentCell() : nullptr;
	if (!cell) {
		if (grid.Snapshot()->requested.front().scope)
			ResetPreparation();
		return;
	}
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

}

void DX12Resources::OnSceneTransitionReset(bool opening)
{
	if (opening)
		ResetPreparation();
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

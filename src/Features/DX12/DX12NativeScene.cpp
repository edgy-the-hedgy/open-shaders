#include "DX12NativeScene.h"

#include "DX12Control.h"
#include "Features/LightLimitFix/Common.h"
#include "Features/LightLimitFix/ShadowCasterManager.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "GpuPass.h"
#include "RE/N/NiLight.h"
#include "RE/T/TESObjectREFR.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string_view>

bool DX12NativeScene::IsActive() const
{
	return loaded && globals::features::dx12Resources.IsActive() &&
	       globals::features::dx12Control.IsActive() && !globals::game::isVR;
}

void DX12NativeScene::DrawSettings()
{
	const bool active = IsActive();
	const auto grid = globals::features::dx12Resources.GetGrid();
	const auto published = scene.Ready();
	const auto resolved = GetLights();
	const auto coverage = GetCoveredLightIDs();
	const uint32_t scope = grid ? grid->requested.front().scope : 0u;
	const auto timings = globals::features::dx12Resources.GetPreparationStats();
	const auto preparationStatus = globals::features::dx12Resources.GetPreparationStatus();
	struct Progress { uint32_t ready = 0, partial = 0, preparing = 0, waiting = 0, incomplete = 0; };
	const auto progress = [](const auto& residents) {
		Progress values;
		for (const auto& tile : residents) {
			if (!tile)
				continue;
			switch (tile->state) {
			case DX12::Tile::State::Ready: ++values.ready; break;
			case DX12::Tile::State::ReadyPartial: ++values.partial; break;
			case DX12::Tile::State::Preparing: ++values.preparing; break;
			case DX12::Tile::State::RetryWaiting: ++values.waiting; break;
			case DX12::Tile::State::Incomplete: ++values.incomplete; break;
			case DX12::Tile::State::Deferred: ++values.waiting; break;
			default: break;
			}
		}
		return values;
	};
	const auto activeProgress = grid ? progress(grid->resident) : Progress{};
	const auto outerProgress = grid ? progress(grid->predictiveResident) : Progress{};
	const bool currentScene = active && published && grid && scope &&
		published->scope == scope && published->geometryRevision == grid->geometryRevision;
	ImGui::Text("Runtime: %s", active ? "Automatic (DX12 master enabled)" : "Inactive (DX12 master or Resources unavailable)");
	ImGui::Text("Subcell size: %u game units", grid ? grid->size : DX12::kSubcellSizes.front());
	ImGui::SeparatorText("Companion subcell generation");
	if (preparationStatus)
		ImGui::TextWrapped("Last companion result: %s", preparationStatus->c_str());
	ImGui::TextUnformatted("Active 3x3");
	ImGui::ProgressBar(float(activeProgress.ready + activeProgress.partial) / float(DX12::kGridCapacity), ImVec2(-1.0f, 0),
		std::format("{}/{} prepared", activeProgress.ready + activeProgress.partial, DX12::kGridCapacity).c_str());
	ImGui::TextDisabled("Complete coverage: %u | Partial geometry: %u", activeProgress.ready, activeProgress.partial);
	ImGui::TextDisabled("Preparing: %u | Retry/deferred: %u | Incomplete: %u",
		activeProgress.preparing, activeProgress.waiting, activeProgress.incomplete);
	ImGui::TextUnformatted("Predictive outer ring (5x5 excluding 3x3)");
	ImGui::ProgressBar(float(outerProgress.ready + outerProgress.partial) / float(DX12::kPredictiveCapacity), ImVec2(-1.0f, 0),
		std::format("{}/{} prepared", outerProgress.ready + outerProgress.partial, DX12::kPredictiveCapacity).c_str());
	ImGui::TextDisabled("Complete coverage: %u | Partial geometry: %u", outerProgress.ready, outerProgress.partial);
	ImGui::TextDisabled("Preparing: %u | Retry/deferred: %u | Incomplete: %u",
		outerProgress.preparing, outerProgress.waiting, outerProgress.incomplete);
	const auto displayTiming = [](const char* label, const DX12::PreparationTiming& timing) {
		if (timing.completed)
			ImGui::Text("%s: last %.1f ms | average %.1f ms | attempts %llu | invalid %llu | partial %llu",
				label, timing.lastMs, timing.AverageMs(),
				static_cast<unsigned long long>(timing.completed),
				static_cast<unsigned long long>(timing.failed),
				static_cast<unsigned long long>(timing.partial));
		else
			ImGui::Text("%s: last -- | average -- | no completed attempts", label);
	};
	if (timings) {
		displayTiming("Active 3x3 preparation", timings->active);
		displayTiming("Predictive ring preparation", timings->predictive);
	}
	ImGui::TextDisabled("Partial subcells can build BLAS/TLAS, but cannot certify static-shadow offloading.");
	ImGui::TextDisabled("Times include companion IPC and worker parsing; canceled requests are excluded.");
	ImGui::SeparatorText("Native acceleration and lighting");
	ImGui::Text("Acceleration scene: %s", currentScene ? "Published" :
		(active && scope ? "Waiting for prepared geometry / BLAS / TLAS" : "Not available"));
	if (currentScene) {
		ImGui::Text("BLAS: %zu  |  TLAS: %s", published->bottoms.size(), published->complete ? "Complete" : "Partial");
	}
	uint32_t matched = 0;
	if (resolved)
		for (uint32_t i = 0; i < resolved->count; ++i)
			matched += resolved->values[i].matched ? 1u : 0u;
	ImGui::Text("Matched prepared lights: %u", matched);
	ImGui::Text("Verified covered lights: %zu", coverage ? coverage->size() : size_t(0));
	ImGui::SeparatorText("Live renderer geometry");
	const size_t captured = grid ? grid->runtimeShapes.size() : 0;
	size_t built = 0;
	if (grid && currentScene)
		for (const auto& observed : grid->runtimeShapes)
			if (observed.shape && published->shapeIDs.contains(observed.shape->identity))
				++built;
	ImGui::Text("Observed static shapes: %zu | BLAS-ready in TLAS: %zu", captured, built);
	ImGui::Text("Runtime CPU geometry: %.2f MiB", grid ? double(grid->runtimeBytes) / (1024.0 * 1024.0) : 0.0);
	ImGui::TextDisabled("Only rigid opaque CPU meshes; bounded to 128 shapes / 24 MiB in active 3x3.");
	ImGui::TextDisabled("Automatically evicted on recenter; no disk cache or authority to suppress raster shadows.");
	ImGui::TextDisabled("BLAS/TLAS construction and light matching are automatic; no user controls here.");
}

void DX12NativeScene::ObserveRuntimeGeometry(RE::BSGeometry* geometry)
{
	if (!geometry || !IsActive() || geometry->name.empty())
		return;
	const auto* reference = geometry->GetUserData();
	if (!reference || reference->IsDynamicForm())
		return;
	const auto* tri = geometry->AsTriShape();
	if (!tri || geometry->AsDynamicTriShape())
		return;
	const auto& runtime = geometry->GetGeometryRuntimeData();
	const auto* renderer = runtime.rendererData;
	if (runtime.skinInstance || runtime.alphaProperty || !renderer ||
		!renderer->rawVertexData || !renderer->rawIndexData ||
		!runtime.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
		return;
	const auto& counts = tri->GetTrishapeRuntimeData();
	// CommonLibSSE VertexDesc::GetSize is non-const; inspect a local copy
	// rather than mutating or casting away const on live renderer data.
	auto vertexDesc = runtime.vertexDesc;
	const uint32_t stride = vertexDesc.GetSize();
	if (!counts.vertexCount || counts.vertexCount > 65535 || !counts.triangleCount ||
		counts.triangleCount > 65535 || stride < 16 || stride > 256)
		return;
	const uint64_t vertexBytes = uint64_t(counts.vertexCount) * stride;
	const uint64_t indexCount = uint64_t(counts.triangleCount) * 3;
	const uint64_t bytes = vertexBytes + indexCount * sizeof(uint16_t);
	if (bytes > 256ull * 1024)
		return;
	const auto bound = geometry->worldBound;
	if (!std::isfinite(bound.center.x) || !std::isfinite(bound.center.y) ||
		!std::isfinite(bound.center.z) || !std::isfinite(bound.radius) || bound.radius <= 0)
		return;
	const auto grid = globals::features::dx12Resources.GetGrid();
	if (!grid || !grid->requested.front().scope || grid->runtimeShapes.size() >= 128)
		return;
	const auto tileX = DX12::Coordinate(bound.center.x, grid->size);
	const auto tileY = DX12::Coordinate(bound.center.y, grid->size);
	if (!tileX || !tileY || !std::ranges::contains(grid->requested,
		DX12::SubcellKey{ grid->requested.front().scope, *tileX, *tileY }))
		return;

	// Renderer object identity is scoped to the live scene. Include the owner,
	// renderer object address, and shape name to avoid merging distinct placed
	// shapes with identical names. Only the numeric identity leaves this hook.
	uint64_t identity = 14695981039346656037ull;
	const auto mix = [&](uint64_t value) {
		identity ^= value;
		identity *= 1099511628211ull;
	};
	mix(reference->GetFormID());
	mix(uint64_t(reinterpret_cast<uintptr_t>(geometry)));
	const std::string_view shapeName{ geometry->name.c_str() };
	if (shapeName.size() > 1024)
		return;
	for (const unsigned char ch : shapeName)
		mix(ch);
	identity |= 0x8000000000000000ull;
	if (std::ranges::any_of(grid->runtimeShapes, [&](const DX12::RuntimeShape& item) {
		return item.shape && item.shape->identity == identity;
	}))
		return;

	const uint32_t frame = globals::game::graphicsState ?
		globals::game::graphicsState->GetFrameCount() : 0;
	{
		std::scoped_lock lock(captureMutex);
		if (frame != captureFrame) {
			captureFrame = frame;
			captureFrameBytes = 0;
			captureFrameShapes = 0;
		}
		if (captureFrameShapes >= 4 || bytes > 512ull * 1024 - captureFrameBytes)
			return;
		++captureFrameShapes;
		captureFrameBytes += uint32_t(bytes);
	}

	auto shape = std::make_shared<DX12::Shape>();
	shape->identity = identity;
	shape->positions.reserve(counts.vertexCount);
	shape->indices.reserve(size_t(indexCount));
	for (uint32_t i = 0; i < counts.vertexCount; ++i) {
		float xyz[3];
		std::memcpy(xyz, renderer->rawVertexData + size_t(i) * stride, sizeof(xyz));
		if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2]))
			return;
		shape->positions.push_back({ xyz[0], xyz[1], xyz[2] });
	}
	for (size_t i = 0; i < indexCount; ++i) {
		const uint32_t index = renderer->rawIndexData[i];
		if (index >= counts.vertexCount)
			return;
		shape->indices.push_back(index);
	}
	for (uint32_t row = 0; row < 3; ++row)
		for (uint32_t col = 0; col < 3; ++col)
			shape->transform[4 * row + col] = geometry->world.rotate.entry[row][col] * geometry->world.scale;
	shape->transform[3] = geometry->world.translate.x;
	shape->transform[7] = geometry->world.translate.y;
	shape->transform[11] = geometry->world.translate.z;
	if (!std::ranges::all_of(shape->transform, [](float value) { return std::isfinite(value); }))
		return;
	// The ResourceGrid verifies current 3x3 ownership and unique identity
	// again under its lock, then publishes an owned immutable geometry snapshot.
	(void)globals::features::dx12Resources.PublishRuntimeShape(
		std::move(shape), bound.center.x, bound.center.y);
}

bool DX12NativeScene::HasCurrentRuntimeGeometry(RE::BSGeometry& geometry) const
{
	if (!IsActive() || geometry.name.empty() ||
		geometry.GetGeometryRuntimeData().skinInstance || geometry.GetGeometryRuntimeData().alphaProperty)
		return false;
	const auto* owner = geometry.GetUserData();
	if (!owner || owner->IsDynamicForm() || std::string_view{ geometry.name.c_str() }.size() > 1024)
		return false;
	const auto grid = globals::features::dx12Resources.GetGrid();
	const auto built = scene.Ready();
	if (!grid || !built || !built->acceleration || !grid->requested.front().scope ||
		built->geometryRevision != grid->geometryRevision || built->scope != grid->requested.front().scope)
		return false;
	uint64_t identity = 14695981039346656037ull;
	const auto mix = [&](uint64_t value) {
		identity ^= value;
		identity *= 1099511628211ull;
	};
	mix(owner->GetFormID());
	mix(uint64_t(reinterpret_cast<uintptr_t>(&geometry)));
	for (const unsigned char ch : std::string_view{ geometry.name.c_str() })
		mix(ch);
	identity |= 0x8000000000000000ull;
	if (!built->shapeIDs.contains(identity))
		return false;
	return std::ranges::any_of(grid->runtimeShapes, [&](const DX12::RuntimeShape& observed) {
		if (!observed.shape || observed.shape->identity != identity)
			return false;
		const auto& transform = observed.shape->transform;
		for (int row = 0; row < 3; ++row)
			for (int col = 0; col < 3; ++col)
				if (std::abs(transform[4 * row + col] -
					geometry.world.rotate.entry[row][col] * geometry.world.scale) > 0.01f)
					return false;
		return std::abs(transform[3] - geometry.world.translate.x) <= 0.01f &&
			std::abs(transform[7] - geometry.world.translate.y) <= 0.01f &&
			std::abs(transform[11] - geometry.world.translate.z) <= 0.01f;
	});
}

bool DX12NativeScene::HasMatchedPreparedLight(const RE::BSShadowLight* light) const
{
	const auto match = matchedShadowLightPointers.load(std::memory_order_acquire);
	return light && match && std::ranges::binary_search(*match, reinterpret_cast<uintptr_t>(light));
}

void DX12NativeScene::PreShadowMaps()
{
	CS_GPU_PASS("DX12NativeScene::Update");
	if (!IsActive()) {
		if (scene.Ready() || lights.load(std::memory_order_acquire))
			OnRuntimeDisabled();
		return;
	}
	const auto grid = globals::features::dx12Resources.GetGrid();
	const auto& swapChain = globals::features::upscaling.dx12SwapChain;
	if (grid && scene.Start(swapChain.d3d12Device.get()))
		scene.Request(grid);
	UpdateLights();
	if (grid)
		PublishCoverage(*grid);
}

void DX12NativeScene::UpdateLights()
{
	const auto grid = globals::features::dx12Resources.GetGrid();
	const uint32_t scope = grid ? grid->requested.front().scope : 0;
	if (!grid || preparedCacheRevision != grid->lightRevision || preparedCacheScope != scope) {
		preparedCache = globals::features::dx12Resources.GetPreparedLights();
		preparedCacheRevision = grid ? grid->lightRevision : std::numeric_limits<uint64_t>::max();
		preparedCacheScope = scope;
	}
	std::vector<DX12::RuntimeLight> runtime;
	if (ShadowCasterManager::IsActive()) {
		const auto& active = ShadowCasterManager::GetLights();
		for (int32_t i = 0; active.Lights && i < active.Size; ++i) {
			auto* shadow = active.Lights[i].Light;
			const auto* source = shadow ? shadow->light.get() : nullptr;
			if (!source || shadow->GetIsDirectionalLight())
				continue;
			const auto* properties = LLFCommon::RuntimeLightDataExt::Get(const_cast<RE::NiLight*>(source));
			const auto* owner = source->GetUserData();
			const auto* reference = owner ? owner->As<RE::TESObjectREFR>() : nullptr;
			const auto& origin = source->world.translate;
			const int32_t slot = ShadowCasterManager::GetShadowSlot(shadow);
			DX12::RuntimeLight light{};
			light.referenceID = reference ? reference->GetFormID() : 0;
			light.baseID = properties ? properties->lighFormId : 0;
			light.shadowSlot = slot >= 0 ? uint32_t(slot) : UINT32_MAX;
			light.moving = shadow->dynamic;
			light.position = { origin.x, origin.y, origin.z };
			if (properties) {
				light.color = { properties->diffuse.red, properties->diffuse.green, properties->diffuse.blue };
				light.radius = properties->radius;
				light.fade = properties->fade;
				light.emitterRadius = properties->size;
			}
			runtime.push_back(light);
		}
	}
	const auto camera = globals::game::player ? globals::game::player->GetPosition() : RE::NiPoint3{};
	const float3 cameraPosition{ camera.x, camera.y, camera.z };
	auto resolved = DX12::MatchPreparedLights(preparedCache, runtime, DX12::LightTuning{}, cameraPosition);
	lights.store(std::make_shared<const DX12::ResolvedLights>(std::move(resolved)), std::memory_order_release);
}

void DX12NativeScene::PublishCoverage(const DX12::Grid& grid)
{
	std::vector<RE::BSShadowLight*> covered;
	std::vector<uint32_t> coveredReferences;
	std::vector<uintptr_t> matchedPointers;
	const auto currentScene = scene.Ready();
	const auto currentLights = lights.load(std::memory_order_acquire);
	if (currentScene && currentLights && currentScene->geometryRevision == grid.geometryRevision &&
		currentScene->scope == grid.requested.front().scope && ShadowCasterManager::IsActive()) {
		const auto& active = ShadowCasterManager::GetLights();
		for (uint32_t i = 0; i < currentLights->count; ++i) {
			const auto& resolved = currentLights->values[i];
			if (!resolved.matched || resolved.light.fade <= 0 ||
				(resolved.light.color.x <= 0 && resolved.light.color.y <= 0 && resolved.light.color.z <= 0) ||
				resolved.runtimeShadowSlot >= uint32_t(std::max(active.Size, 0)) ||
				!active.Lights || currentScene->shapeIDs.empty())
				continue;
			auto* runtimeShadow = active.Lights[resolved.runtimeShadowSlot].Light;
			if (!runtimeShadow)
				continue;
			matchedPointers.push_back(reinterpret_cast<uintptr_t>(runtimeShadow));
			const auto& light = resolved.light;
			const auto x0 = DX12::Coordinate(double(light.position.x) - light.radius, grid.size);
			const auto x1 = DX12::Coordinate(double(light.position.x) + light.radius, grid.size);
			const auto y0 = DX12::Coordinate(double(light.position.y) - light.radius, grid.size);
			const auto y1 = DX12::Coordinate(double(light.position.y) + light.radius, grid.size);
			if (!x0 || !x1 || !y0 || !y1 || *x1 - *x0 > 2 || *y1 - *y0 > 2)
				continue;
			bool fullyCovered = true;
			for (int64_t y = *y0; y <= *y1 && fullyCovered; ++y)
				for (int64_t x = *x0; x <= *x1; ++x) {
					const auto tile = std::ranges::find(grid.requested, DX12::SubcellKey{ grid.requested.front().scope, x, y });
					if (tile == grid.requested.end()) {
						fullyCovered = false;
						break;
					}
					const auto& resident = grid.resident[std::distance(grid.requested.begin(), tile)];
					if (!resident || resident->state != DX12::Tile::State::Ready ||
						!std::ranges::all_of(resident->shapes, [&](const auto& shape) {
							return currentScene->shapeIDs.contains(shape->identity);
						})) {
						fullyCovered = false;
						break;
					}
				}
			if (fullyCovered)
				if (auto* shadow = active.Lights[resolved.runtimeShadowSlot].Light) {
					covered.push_back(shadow);
					coveredReferences.push_back(light.referenceID);
				}
		}
	}
	std::ranges::sort(matchedPointers);
	matchedPointers.erase(std::unique(matchedPointers.begin(), matchedPointers.end()), matchedPointers.end());
	matchedShadowLightPointers.store(std::make_shared<const std::vector<uintptr_t>>(std::move(matchedPointers)),
		std::memory_order_release);
	std::ranges::sort(coveredReferences);
	coveredReferences.erase(std::unique(coveredReferences.begin(), coveredReferences.end()), coveredReferences.end());
	coveredLightIDs.store(std::make_shared<const std::vector<uint32_t>>(std::move(coveredReferences)), std::memory_order_release);
	ShadowCasterManager::PublishNativeStaticCoverage(covered);
}

void DX12NativeScene::OnSceneTransitionReset(bool opening)
{
	if (opening)
		OnRuntimeDisabled();
}

void DX12NativeScene::OnRuntimeDisabled()
{
	{
		std::scoped_lock lock(captureMutex);
		captureFrame = UINT32_MAX;
		captureFrameBytes = captureFrameShapes = 0;
	}
	scene.Clear();
	preparedCache.clear();
	preparedCacheRevision = std::numeric_limits<uint64_t>::max();
	preparedCacheScope = 0;
	lights.store(nullptr, std::memory_order_release);
	coveredLightIDs.store(nullptr, std::memory_order_release);
	matchedShadowLightPointers.store(nullptr, std::memory_order_release);
	ShadowCasterManager::SetNativeStaticMaskActive(false);
	ShadowCasterManager::PublishNativeStaticCoverage({});
}

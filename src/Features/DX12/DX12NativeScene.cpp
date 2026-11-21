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
#include <format>

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
	struct Progress { uint32_t ready = 0, preparing = 0, waiting = 0, incomplete = 0; };
	const auto progress = [](const auto& residents) {
		Progress values;
		for (const auto& tile : residents) {
			if (!tile)
				continue;
			switch (tile->state) {
			case DX12::Tile::State::Ready: ++values.ready; break;
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
	ImGui::TextUnformatted("Active 3x3");
	ImGui::ProgressBar(float(activeProgress.ready) / float(DX12::kGridCapacity), ImVec2(-1.0f, 0),
		std::format("{}/{} ready", activeProgress.ready, DX12::kGridCapacity).c_str());
	ImGui::TextDisabled("Preparing: %u | Retry/deferred: %u | Incomplete: %u",
		activeProgress.preparing, activeProgress.waiting, activeProgress.incomplete);
	ImGui::TextUnformatted("Predictive outer ring (5x5 excluding 3x3)");
	ImGui::ProgressBar(float(outerProgress.ready) / float(DX12::kPredictiveCapacity), ImVec2(-1.0f, 0),
		std::format("{}/{} ready", outerProgress.ready, DX12::kPredictiveCapacity).c_str());
	ImGui::TextDisabled("Preparing: %u | Retry/deferred: %u | Incomplete: %u",
		outerProgress.preparing, outerProgress.waiting, outerProgress.incomplete);
	const auto displayTiming = [](const char* label, const DX12::PreparationTiming& timing) {
		if (timing.completed)
			ImGui::Text("%s: last %.1f ms | average %.1f ms | attempts %llu | failed %llu",
				label, timing.lastMs, timing.AverageMs(),
				static_cast<unsigned long long>(timing.completed),
				static_cast<unsigned long long>(timing.failed));
		else
			ImGui::Text("%s: last -- | average -- | no completed attempts", label);
	};
	if (timings) {
		displayTiming("Active 3x3 preparation", timings->active);
		displayTiming("Predictive ring preparation", timings->predictive);
	}
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
	ImGui::TextDisabled("BLAS/TLAS construction and light matching are automatic; no user controls here.");
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
	scene.Clear();
	preparedCache.clear();
	preparedCacheRevision = std::numeric_limits<uint64_t>::max();
	preparedCacheScope = 0;
	lights.store(nullptr, std::memory_order_release);
	coveredLightIDs.store(nullptr, std::memory_order_release);
	ShadowCasterManager::SetNativeStaticMaskActive(false);
	ShadowCasterManager::PublishNativeStaticCoverage({});
}

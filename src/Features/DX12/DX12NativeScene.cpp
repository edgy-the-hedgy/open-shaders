#include "DX12NativeScene.h"

#include "DX12Control.h"
#include "DX12NativeComposite.h"
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

void DX12NativeScene::DrawGeometryDiagnostics()
{
	const bool active = IsActive();
	const auto grid = globals::features::dx12Resources.GetGrid();
	const auto published = scene.Ready();
	const auto resolved = GetLights();
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
	ImGui::TextDisabled("Partial subcells remain usable for RayQuery; no per-light coverage certification is required.");
	ImGui::TextDisabled("Times include companion IPC and worker parsing; canceled requests are excluded.");
	ImGui::SeparatorText("Native acceleration and prepared lights");
	ImGui::Text("Acceleration scene: %s", currentScene ? "Published" :
		(active && scope ? "Waiting for prepared geometry / BLAS / TLAS" : "Not available"));
	if (currentScene) {
		ImGui::Text("BLAS: %zu  |  TLAS: %s", published->bottoms.size(), published->complete ? "Complete" : "Partial");
	}
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

void DX12NativeScene::DrawSettings()
{
	const auto resolved = GetLights();
	uint32_t matched = 0;
	if (resolved)
		for (uint32_t i = 0; i < resolved->count; ++i)
			matched += resolved->values[i].matched ? 1u : 0u;
	ImGui::SeparatorText("Prepared-light characteristics");
	bool inherit = inheritRuntime.load(std::memory_order_relaxed);
	if (ImGui::Checkbox("Inherit matched LLF/SCM characteristics", &inherit))
		inheritRuntime.store(inherit, std::memory_order_release);
	ImGui::TextDisabled("With inheritance off, prepared positions and user-defined multipliers take precedence.");
	float reach = lightReach.load(std::memory_order_relaxed);
	if (ImGui::SliderFloat("Native shadow reach", &reach, 0.1f, 4.0f, "%.2fx"))
		lightReach.store(reach, std::memory_order_release);
	float intensity = lightIntensity.load(std::memory_order_relaxed);
	if (ImGui::SliderFloat("Prepared light intensity", &intensity, 0.0f, 8.0f, "%.2fx"))
		lightIntensity.store(intensity, std::memory_order_release);
	float emitter = emitterSize.load(std::memory_order_relaxed);
	if (ImGui::SliderFloat("Prepared emitter size", &emitter, 0.1f, 8.0f, "%.2fx"))
		emitterSize.store(emitter, std::memory_order_release);
	float match = lightMatchDistance.load(std::memory_order_relaxed);
	if (ImGui::SliderFloat("Runtime light matching distance", &match, 1.0f, 256.0f, "%.0f"))
		lightMatchDistance.store(match, std::memory_order_release);
	ImGui::SeparatorText("Diagnostics");
	ImGui::Text("Matched LLF/SCM lights: %u", matched);
	ImGui::TextDisabled("Matched lights inherit SCM values except when authored flicker/pulse is forced static.");
	ImGui::TextDisabled("Unmatched prepared lights retain companion values; no native direct-light pass required.");
	ImGui::Text("Prepared lights for native tracing: %u", resolved ? resolved->count : 0u);

}
void DX12NativeScene::LoadSettings(json& input)
{
    inheritRuntime.store(input.value("InheritRuntime", true), std::memory_order_release);
    lightReach.store(std::clamp(input.value("Reach", 1.0f), 0.1f, 4.0f), std::memory_order_release);
    lightIntensity.store(std::clamp(input.value("Intensity", 1.0f), 0.0f, 8.0f), std::memory_order_release);
    emitterSize.store(std::clamp(input.value("EmitterSize", 1.0f), 0.1f, 8.0f), std::memory_order_release);
    lightMatchDistance.store(std::clamp(input.value("MatchDistance", 64.0f), 1.0f, 256.0f), std::memory_order_release);
}
void DX12NativeScene::SaveSettings(json& output)
{
    output = {{"InheritRuntime", inheritRuntime.load(std::memory_order_acquire)},
        {"Reach", lightReach.load(std::memory_order_acquire)},
        {"Intensity", lightIntensity.load(std::memory_order_acquire)},
        {"EmitterSize", emitterSize.load(std::memory_order_acquire)},
        {"MatchDistance", lightMatchDistance.load(std::memory_order_acquire)}};
}
void DX12NativeScene::RestoreDefaultSettings()
{
    inheritRuntime.store(true, std::memory_order_release);
    lightReach.store(1.0f, std::memory_order_release);
    lightIntensity.store(1.0f, std::memory_order_release);
    emitterSize.store(1.0f, std::memory_order_release);
    lightMatchDistance.store(64.0f, std::memory_order_release);
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
		built->scope != grid->requested.front().scope)
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

bool DX12NativeScene::IsMatchedLightInShadowMode(const RE::BSShadowLight* light, bool stationary, bool flickering) const
{
	if (!light || (!stationary && !flickering) || !HasMatchedPreparedLight(light))
		return false;
	const auto knownFlickering = flickeringShadowLightPointers.load(std::memory_order_acquire);
	const bool flickers = knownFlickering && std::ranges::binary_search(*knownFlickering,
		reinterpret_cast<uintptr_t>(light));
	return flickers ? flickering : stationary;
}

void DX12NativeScene::PreShadowMaps()
{
	CS_GPU_PASS("DX12NativeScene::Update");
	if (!IsActive()) {
		if (scene.Ready() || lights.load(std::memory_order_acquire))
			OnRuntimeDisabled();
		return;
	}
	const uint64_t flush = globals::features::dx12Resources.GetFlushGeneration();
	if (knownFlushGeneration != flush) {
		scene.Clear();
		knownFlushGeneration = flush;
	}
	const auto grid = globals::features::dx12Resources.GetGrid();
	const auto& swapChain = globals::features::upscaling.dx12SwapChain;
	if (grid && scene.Start(swapChain.d3d12Device.get()))
		scene.Request(grid);
	const auto now = std::chrono::steady_clock::now();
	if (lastLightsRefresh.time_since_epoch().count() == 0 ||
		now - lastLightsRefresh >= std::chrono::milliseconds(150)) {
		UpdateLights();
		lastLightsRefresh = now;
	}
	PublishMatchedLights();
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
	DX12::LightTuning tuning{};
	tuning.inheritRuntime = inheritRuntime.load(std::memory_order_acquire);
	tuning.forceStaticFlicker = globals::features::dx12NativeComposite.ForceStaticPreparedFlicker();
	tuning.matchDistance = lightMatchDistance.load(std::memory_order_acquire);
	tuning.reach = lightReach.load(std::memory_order_acquire);
	tuning.intensity = lightIntensity.load(std::memory_order_acquire);
	tuning.emitterSize = emitterSize.load(std::memory_order_acquire);
	auto resolved = DX12::MatchPreparedLights(preparedCache, runtime, tuning, cameraPosition);
	lights.store(std::make_shared<const DX12::ResolvedLights>(std::move(resolved)), std::memory_order_release);
}

void DX12NativeScene::PublishMatchedLights()
{
	// Matching helps SCM identify a prepared light. It never admits or
	// rejects native RayQuery lights: those come directly from the complete
	// prepared-light snapshot, including off-screen and predictive emitters.
	std::vector<uintptr_t> matchedPointers;
	std::vector<uintptr_t> flickeringPointers;
	const auto currentLights = lights.load(std::memory_order_acquire);
	if (currentLights && ShadowCasterManager::IsActive()) {
		const auto& active = ShadowCasterManager::GetLights();
		if (active.Lights) {
			for (const auto& resolved : currentLights->values) {
				if (!resolved.matched ||
					resolved.runtimeShadowSlot >= uint32_t(std::max(active.Size, 0)))
					continue;
				auto* shadow = active.Lights[resolved.runtimeShadowSlot].Light;
				if (!shadow)
					continue;
				matchedPointers.push_back(reinterpret_cast<uintptr_t>(shadow));
				if (resolved.flickeringOrMoving)
					flickeringPointers.push_back(reinterpret_cast<uintptr_t>(shadow));
			}
		}
	}
	std::ranges::sort(matchedPointers);
	matchedPointers.erase(std::unique(matchedPointers.begin(), matchedPointers.end()), matchedPointers.end());
	matchedShadowLightPointers.store(std::make_shared<const std::vector<uintptr_t>>(std::move(matchedPointers)),
		std::memory_order_release);
	std::ranges::sort(flickeringPointers);
	flickeringPointers.erase(std::unique(flickeringPointers.begin(), flickeringPointers.end()), flickeringPointers.end());
	flickeringShadowLightPointers.store(std::make_shared<const std::vector<uintptr_t>>(std::move(flickeringPointers)),
		std::memory_order_release);
	// This legacy SCM coverage list is not a native light-admission test.
	// Do not certify raster-shadow suppression until native per-light composition
	// can replace the static shadow contribution in the output image.
	ShadowCasterManager::PublishNativeStaticCoverage({});
}

void DX12NativeScene::OnSceneTransitionReset(bool /*opening*/)
{
	// Preserve the last completed TLAS until the replacement is published.
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
	lastLightsRefresh = {};
	lights.store(nullptr, std::memory_order_release);
	matchedShadowLightPointers.store(nullptr, std::memory_order_release);
	flickeringShadowLightPointers.store(nullptr, std::memory_order_release);
	ShadowCasterManager::SetNativeStaticMaskActive(false);
	ShadowCasterManager::PublishNativeStaticCoverage({});
}

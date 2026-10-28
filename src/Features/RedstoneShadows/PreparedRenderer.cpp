#include "RedstoneShadows.h"

#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Util.h"

void RedstoneShadows::ResetPreparedScene()
{
	++preparedRevision;
	preparedCellID = 0;
	preparedSubmitted = preparedActive = false;
	preparedTraceFrame.reset();
	preparedScene.Reset();
	preparedAcceleration.Reset();
	preparedTrace.Reset();
}

void RedstoneShadows::AdvancePreparedScene(uint32_t, uint64_t a_budgetBytes)
{
	CS_GPU_PASS("RedstoneShadows::PreparedSceneProgress");
	auto* cell = globals::game::player ? globals::game::player->GetParentCell() : nullptr;
	const auto cellID = cell ? cell->GetFormID() : 0;
	const bool active = ready && !transitionOpen && !globals::game::isVR && !globals::state->isMapMenuOpen && cell && cell->IsInteriorCell();
	if (resetRequested || cellID != preparedCellID || active != preparedActive) {
		ResetPreparedScene();
		preparedCellID = cellID;
		preparedActive = active;
		resetRequested = false;
	}
	if (!active) return;
	if (preparedSubmitted) return;
	if (preparedScene.Requested() && !preparedScene.GetCell()) return;
	const auto backend = Feature::FindGraphicsBackend();
	if (!backend || !backend->commandQueue) return;
	if (!preparedScene.Requested()) {
		const auto* source = cell->GetFile(0);
		if (!source) return;
		const auto local = cell->GetLocalFormID();
		const auto key = std::format("{:06X}:{}", local, source->GetFilename());
		const auto directory = std::filesystem::path("Data/SKSE/Plugins/CommunityShaders/RedstoneScene/cells") /
			source->GetFilename() / std::format("{:06X}", local);
		preparedScene.Request(*backend, directory, key, preparedRevision, a_budgetBytes);
	}
	if (!preparedSubmitted) {
		if (auto prepared = preparedScene.GetCell()) {
			preparedAcceleration.Start(*backend, prepared->snapshot, prepared->geometries, a_budgetBytes);
			preparedSubmitted = true;
		}
	}
	preparedAcceleration.Advance();
}

void RedstoneShadows::OnWorldRenderEnd(RE::RENDER_TARGET)
{
	CS_GPU_PASS("RedstoneShadows::PreparedLocalTrace");
	constexpr uint32_t kPreviewInterval = 8;
	if (!settings.UsePreparedScene || !settings.PreviewPreparedScene || !preparedActive || transitionOpen || globals::game::isVR || globals::state->isMapMenuOpen) return;
	const auto frame = globals::game::graphicsState->GetFrameCount();
	if (frame % kPreviewInterval || preparedTraceFrame == frame) return;
	auto* cell = globals::game::player ? globals::game::player->GetParentCell() : nullptr;
	if (!cell || cell->GetFormID() != preparedCellID) { preparedTrace.Reset(); return; }
	const auto prepared = preparedScene.GetCell();
	const auto completedScene = preparedAcceleration.GetScene();
	if (!prepared || !completedScene || completedScene->scope.scene != preparedRevision || settings.PreparedLight >= prepared->lights.size()) return;
	const auto backend = Feature::FindGraphicsBackend();
	if (!backend || !preparedTrace.Initialize(*backend)) return;
	const auto resolution = Util::ConvertToDynamic(globals::state->screenSize);
	preparedTrace.Dispatch(completedScene, { uint32_t(resolution.x), uint32_t(resolution.y) }, prepared->lights[settings.PreparedLight],
		frame, uint64_t(settings.BufferBudgetMB) * 1024 * 1024 / 4, true);
	preparedTraceFrame = frame;
}

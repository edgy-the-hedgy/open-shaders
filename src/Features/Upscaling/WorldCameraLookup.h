#pragma once

#include "Globals.h"
#include "CameraReprojection.h"
#include <optional>

namespace UpscalingCamera
{
	// Flat (SE/AE) layout of a BSGraphics::State::cameraDataCacheA entry; the universal CommonLib build compiles these fields out.
	struct CameraStateEntry
	{
		RE::NiCamera* referenceCamera;
		std::uint64_t pad08;
		RE::BSGraphics::ViewData viewData;
		RE::NiPoint3 posAdjust;
		RE::NiPoint3 currentPosAdjust;
		RE::NiPoint3 previousPosAdjust;
		bool useJitter;
		std::uint32_t numData;
		std::uint32_t pad28C;
	};
	static_assert(offsetof(CameraStateEntry, viewData) == 0x10);
	static_assert(offsetof(CameraStateEntry, posAdjust) == 0x260);
	static_assert(offsetof(CameraStateEntry, previousPosAdjust) == 0x278);
	static_assert(sizeof(CameraStateEntry) == 0x290);

	/**
	 * @brief The world camera's frame from the engine camera table, which keys entries on (camera, useJitter):
	 * the jittered entry the scene draws with, else the unjittered one; nullopt in VR or when neither exists.
	 */
	inline std::optional<CameraFrame> GetWorldCameraFrame()
	{
		const auto* camera = RE::Main::WorldRootCamera();
		if (globals::game::isVR || !camera || !globals::game::graphicsState)
			return std::nullopt;
		const auto& table = globals::game::graphicsState->GetRuntimeData().cameraDataCacheA;
		const auto* entries = reinterpret_cast<const CameraStateEntry*>(table.data());
		const CameraStateEntry* match = nullptr;
		for (std::uint32_t i = 0; i < table.size(); ++i) {
			const auto& entry = entries[i];
			if (entry.referenceCamera != camera)
				continue;
			match = &entry;
			if (entry.useJitter)
				break;
		}
		if (!match)
			return std::nullopt;
		const auto& view = match->viewData;
		return UpscalingCamera::FromCameraTable(view.viewMat, view.viewProjMatrixUnjittered, view.previousViewProjMatrixUnjittered,
			float3(match->posAdjust.x, match->posAdjust.y, match->posAdjust.z),
			float3(match->previousPosAdjust.x, match->previousPosAdjust.y, match->previousPosAdjust.z));
	}
}


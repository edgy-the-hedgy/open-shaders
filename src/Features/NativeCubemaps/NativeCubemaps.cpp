#include "NativeCubemaps.h"

#include "Features/DynamicCubemaps.h"
#include "Features/IBL.h"
#include "Features/LinearLighting.h"
#include "Globals.h"
#include "State.h"
#include "Utils/Game.h"

#include <cmath>

DX12Native::EnvironmentStamp NativeCubemaps::Producer::Prepare(uint32_t scope, uint32_t frame, bool& changed)
{
	using namespace DX12Native;
	const auto color = globals::features::linearLighting.GetCommonBufferData();
	const auto encoding = color.enableLinearLighting ?
		(color.enableACEScg ? EnvironmentEncoding::LinearAP1 : EnvironmentEncoding::LinearRec709) :
		EnvironmentEncoding::LegacyScene;
	EnvironmentStamp stamp{ epoch, scope, frame, globals::state->frameCount, encoding, Util::IsInterior() };
	const float hours = globals::game::calendar ? globals::game::calendar->GetHoursPassed() : lastHours;
	changed = currentStamp.epoch && (!currentStamp.SameEnvironment(stamp) ||
		(std::isfinite(hours) && std::abs(hours - lastHours) > 0.01f));
	if (changed) {
		InvalidateGeneration();
		stamp.epoch = epoch;
	}
	currentStamp = stamp;
	if (std::isfinite(hours))
		lastHours = hours;
	// Transitional capture adapters; native processing consumes only stamped publications.
	globals::features::dynamicCubemaps.SetNativeCaptureContext(stamp);
	globals::features::ibl.SetNativeCaptureContext(stamp);
	return stamp;
}

void NativeCubemaps::Producer::Capture(const GraphicsBackend& backend)
{
	using namespace DX12Native;
	if (!environment)
		environment = std::make_unique<NativeEnvironment>();
	const auto& cubes = globals::features::dynamicCubemaps;
	const auto& ibl = globals::features::ibl;
	environment->Capture(backend, currentStamp,
		cubes.loaded ? cubes.GetNativeEnvironment(false) : EnvironmentSource{},
		cubes.loaded && !currentStamp.interior ? cubes.GetNativeEnvironment(true) : EnvironmentSource{},
		ibl.loaded && !currentStamp.interior ? ibl.GetNativeSkyRadiance() : EnvironmentSource{});
}

void NativeCubemaps::Producer::InvalidateGeneration()
{
	if (environment)
		environment->Reset();
	if (currentStamp.epoch) {
		globals::features::dynamicCubemaps.SetNativeCaptureContext({});
		globals::features::ibl.SetNativeCaptureContext({});
	}
	currentStamp = {};
	++epoch;
}

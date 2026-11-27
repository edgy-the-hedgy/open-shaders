#include "NativeCubemaps.h"

#include "Features/DynamicCubemaps.h"
#include "Features/NativeIBL/NativeIBL.h"
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
	// The legacy capture adapters retain only coherent source revisions.
	if (!cubeContext.SameEnvironment(stamp)) {
		ResetLegacyCubeBridge();
		auto& cubes = globals::features::dynamicCubemaps;
		cubes.resetCapture[0] = cubes.resetCapture[1] = true;
	}
	cubeContext = stamp;
	globals::features::nativeIBL.SetCaptureContext(stamp);
	return stamp;
}

void NativeCubemaps::Producer::Capture(const GraphicsBackend& backend)
{
	using namespace DX12Native;
	if (!environment)
		environment = std::make_unique<NativeEnvironment>();
	const auto& cubes = globals::features::dynamicCubemaps;
	environment->Capture(backend, currentStamp,
		cubes.loaded ? cubeSources[0].Get(currentStamp) : EnvironmentSource{},
		cubes.loaded && !currentStamp.interior ? cubeSources[1].Get(currentStamp) : EnvironmentSource{},
		!currentStamp.interior ? globals::features::nativeIBL.GetSkySource(currentStamp) : EnvironmentSource{});
}

void NativeCubemaps::Producer::InvalidateGeneration()
{
	if (environment)
		environment->Reset();
	ResetLegacyCubeBridge();
	globals::features::nativeIBL.SetCaptureContext({});
	currentStamp = {};
	++epoch;
}

void NativeCubemaps::Producer::ResetLegacyCubeBridge()
{
	cubeContext = {};
	cubePending = {};
	cubeSources = {};
}

void NativeCubemaps::Producer::RecordLegacyCubeCapture(uint32_t index, bool successful)
{
	if (!cubeContext.epoch || index >= cubePending.size())
		return;
	const bool sameFrame = globals::game::graphicsState && globals::state &&
		cubeContext.engineFrame == globals::game::graphicsState->GetFrameCount() &&
		cubeContext.pluginFrame == globals::state->frameCount;
	cubePending[index] = successful && sameFrame ? cubeContext : DX12Native::EnvironmentStamp{};
}

void NativeCubemaps::Producer::PublishLegacyCube(uint32_t index, ID3D11Texture2D* texture)
{
	if (!cubeContext.epoch || index >= cubePending.size())
		return;
	auto& source = cubeSources[index];
	const auto capture = cubePending[index];
	source = {};
	if (texture && capture.SameEnvironment(cubeContext)) {
		source.texture.copy_from(texture);
		source.stamp = capture;
		source.revision = ++cubeRevision;
	}
	cubePending[index] = {};
}

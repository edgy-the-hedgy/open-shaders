#include "NativeComposite.h"

#include "Globals.h"
#include "Utils/DevBenchUx.h"

void NativeCompositeFeature::DrawSettings()
{
	bool selected = enabled.load(std::memory_order_relaxed);
	if (ImGui::Checkbox("Enable raster DX12 composite", &selected))
		enabled.store(selected, std::memory_order_relaxed);
	ImGui::TextDisabled("Raster pass-through; no DXR, geometry, lighting, or shadows.");
	const auto report = GetDiagnostics();
	ImGui::TextDisabled("Status: %s", report.value("status", std::string("Unavailable")).c_str());
	ImGui::TextDisabled("Captured frames: %llu", static_cast<unsigned long long>(captures.load()));
}

void NativeCompositeFeature::LoadSettings(json& a_json)
{
	enabled.store(a_json.value("Enabled", false), std::memory_order_relaxed);
}

void NativeCompositeFeature::SaveSettings(json& a_json)
{
	a_json = { { "Enabled", enabled.load(std::memory_order_relaxed) } };
}

void NativeCompositeFeature::RestoreDefaultSettings()
{
	enabled.store(false, std::memory_order_relaxed);
}

void NativeCompositeFeature::Reset()
{
	std::scoped_lock lock(backendMutex);
	if (!backend)
		attempted = false;
}

std::shared_ptr<const NativeComposite::Frame> NativeCompositeFeature::Capture(ID3D11Texture2D* a_color,
	uint32_t a_frame, uint32_t a_width, uint32_t a_height, NativeComposite::Transfer a_transfer)
{
	if (!IsEnabled() || !a_color || !a_width || !a_height)
		return {};
	std::scoped_lock lock(backendMutex);
	if (!backend && !attempted) {
		attempted = true;
		backend = GraphicsBackend::Create(globals::d3d::device, globals::d3d::context, backendFailure);
		if (!backend)
			logger::warn("[NativeComposite] Backend unavailable: {}", backendFailure);
	}
	if (!backend)
		return {};
	auto frame = pass.Capture(backend->d3d11Device.get(), backend->d3d11Context.get(),
		backend->d3d12Device.get(), backend->commandQueue.get(), a_color,
		a_frame, a_width, a_height, a_transfer);
	if (frame)
		++captures;
	return frame;
}

json NativeCompositeFeature::GetDiagnostics()
{
	std::scoped_lock lock(backendMutex);
	return {
		{ "enabled", IsEnabled() },
		{ "backendReady", backend.has_value() },
		{ "captures", captures.load() },
		{ "completedFrames", pass.CompletedFrames() },
		{ "status", backend ? pass.Status() : attempted ? backendFailure : "Not initialized" },
		{ "lightingSubmitted", false },
		{ "geometryCount", 0 }
	};
}

void NativeCompositeFeature::RegisterUxActions()
{
	FEATURE_QUERY("nativeCompositeStatus", "Reports DX12 raster pass-through frame capture and backend status.",
		([](const Feature* self, const json&) -> json {
			return const_cast<NativeCompositeFeature*>(static_cast<const NativeCompositeFeature*>(self))->GetDiagnostics();
		}));
}

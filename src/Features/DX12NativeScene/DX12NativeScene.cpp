#include "DX12NativeScene.h"

#include "Globals.h"
#include "Utils/DevBenchUx.h"

void DX12NativeScene::DrawSettings()
{
	bool selected = enabled.load(std::memory_order_relaxed);
	if (ImGui::Checkbox("Enable geometry-free DX12 backend", &selected))
		enabled.store(selected, std::memory_order_relaxed);
	const auto diagnostics = GetDiagnostics();
	ImGui::TextDisabled("Backend: %s", diagnostics.value("status", std::string("Unavailable")).c_str());
	ImGui::TextDisabled("Generation: %llu", static_cast<unsigned long long>(GetGeneration()));
}

void DX12NativeScene::LoadSettings(json& a_json)
{
	enabled.store(a_json.value("Enabled", false), std::memory_order_relaxed);
}

void DX12NativeScene::SaveSettings(json& a_json)
{
	a_json = { { "Enabled", enabled.load(std::memory_order_relaxed) } };
}

void DX12NativeScene::RestoreDefaultSettings()
{
	enabled.store(false, std::memory_order_relaxed);
}

void DX12NativeScene::Prepass()
{
	if (!loaded || !enabled.load(std::memory_order_relaxed) || transitionOpen.load(std::memory_order_relaxed))
		return;
	if (!globals::d3d::device || !globals::d3d::context)
		return;
	std::scoped_lock lock(mutex);
	if (backend || attempted)
		return;
	attempted = true;
	backend = GraphicsBackend::Create(globals::d3d::device, globals::d3d::context, failure);
	if (!backend)
		logger::warn("[DX12NativeScene] Backend unavailable: {}", failure);
}

void DX12NativeScene::Reset()
{
	std::scoped_lock lock(mutex);
	if (!backend)
		attempted = false;
	generation.fetch_add(1, std::memory_order_relaxed);
}

void DX12NativeScene::OnSceneTransitionReset(bool a_open)
{
	transitionOpen.store(a_open, std::memory_order_relaxed);
	generation.fetch_add(1, std::memory_order_relaxed);
}

std::optional<GraphicsBackend> DX12NativeScene::GetGraphicsBackend() const
{
	if (!IsEnabled())
		return std::nullopt;
	std::scoped_lock lock(mutex);
	return backend;
}

bool DX12NativeScene::IsEnabled() const
{
	return loaded && enabled.load(std::memory_order_relaxed) && !transitionOpen.load(std::memory_order_relaxed);
}

json DX12NativeScene::GetDiagnostics()
{
	std::scoped_lock lock(mutex);
	return {
		{ "enabled", enabled.load(std::memory_order_relaxed) },
		{ "transitionOpen", transitionOpen.load(std::memory_order_relaxed) },
		{ "generation", generation.load(std::memory_order_relaxed) },
		{ "backendReady", backend.has_value() },
		{ "status", backend ? "Ready" : attempted ? failure : "Not initialized" },
		{ "geometryCount", 0 }
	};
}

void DX12NativeScene::RegisterUxActions()
{
	FEATURE_QUERY("nativeBackendStatus", "Reports the geometry-free DX12 backend, transition state, and generation.",
		([](const Feature* self, const json&) -> json {
			return const_cast<DX12NativeScene*>(static_cast<const DX12NativeScene*>(self))->GetDiagnostics();
		}));
}

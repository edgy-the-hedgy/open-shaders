#pragma once

#include "Feature.h"
#include "GraphicsBackend.h"
#include "NativeCompositePass.h"

#include <atomic>
#include <mutex>

/** @brief Publishes a raster color frame for native DX12 consumers. */
struct NativeCompositeFeature final : Feature
{
	std::string GetName() override { return "Native Composite"; }
	std::string GetShortName() override { return "NativeComposite"; }
	std::string_view GetCategory() const override { return FeatureCategories::kDisplay; }
	bool SupportsVR() override { return true; }
	bool IsCore() const override { return true; }

	void DrawSettings() override;
	void LoadSettings(json& a_json) override;
	void SaveSettings(json& a_json) override;
	void RestoreDefaultSettings() override;
	void Reset() override;
	void RegisterUxActions() override;
	json GetDiagnostics() override;

	/** @brief Captures the active DX11 scene before an available DX12 upscaler. */
	std::shared_ptr<const NativeComposite::Frame> Capture(ID3D11Texture2D* a_color,
		uint32_t a_frame, uint32_t a_width, uint32_t a_height, NativeComposite::Transfer a_transfer);
	/** @brief Tracks the GPU consumer of the color frame. */
	bool Retain(const std::shared_ptr<const NativeComposite::Frame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value)
	{
		return pass.Retain(a_frame, a_fence, a_value);
	}
	bool IsEnabled() const { return loaded && enabled.load(std::memory_order_relaxed); }

private:
	NativeComposite pass;
	std::optional<GraphicsBackend> backend;
	std::mutex backendMutex;
	std::string backendFailure;
	bool attempted = false;
	std::atomic<bool> enabled{ false };
	std::atomic<uint64_t> captures{ 0 };
};

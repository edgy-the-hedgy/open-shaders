#pragma once

#include "Feature.h"
#include "GraphicsBackend.h"

#include <atomic>
#include <mutex>

/** @brief Owns an optional DX12 backend without loading geometry or ray-tracing resources. */
struct DX12NativeScene final : Feature
{
	std::string GetName() override { return "DX12 Native Scene"; }
	std::string GetShortName() override { return "DX12NativeScene"; }
	std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	bool SupportsVR() override { return true; }
	bool IsCore() const override { return true; }

	void DrawSettings() override;
	void LoadSettings(json& a_json) override;
	void SaveSettings(json& a_json) override;
	void RestoreDefaultSettings() override;
	void Prepass() override;
	void Reset() override;
	void OnSceneTransitionReset(bool a_open) override;
	void RegisterUxActions() override;
	json GetDiagnostics() override;

	/** @brief Copies the adapter-matched device and queue for one frame's submission. */
	std::optional<GraphicsBackend> GetGraphicsBackend() const;
	/** @brief Indicates that native work may be submitted for the current world frame. */
	bool IsEnabled() const;
	/** @brief Identifies a loading transition for consumers that retain frame history. */
	uint64_t GetGeneration() const { return generation.load(std::memory_order_relaxed); }

private:
	mutable std::mutex mutex;
	std::optional<GraphicsBackend> backend;
	std::string failure;
	std::atomic<bool> enabled{ false };
	std::atomic<bool> transitionOpen{ false };
	std::atomic<uint64_t> generation{ 1 };
	bool attempted = false;
};

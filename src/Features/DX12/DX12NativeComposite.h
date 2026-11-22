#pragma once

#include "DX12NativeScene.h"

#include <atomic>
#include <cstdint>
#include <d3d12.h>
#include <deque>
#include <memory>
#include <optional>
#include <thread>
#include <winrt/base.h>

/** @brief Independent RayQuery shadowmask producer; final raster color remains untouched until per-light integration. */
struct DX12NativeComposite final : Feature
{
	~DX12NativeComposite();

	struct Surfaces
	{
		winrt::com_ptr<ID3D12Resource> shadowMask, diagnosticColor, lightSpaceAtlas;
		uint64_t atlasSignature = 0;
		bool atlasValid = false;
		winrt::com_ptr<ID3D12Resource> rasterSource, depthSource;
		winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
		uint32_t width = 0, height = 0;
		DXGI_FORMAT rasterFormat = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
	};

	struct Submission
	{
		std::shared_ptr<Surfaces> surfaces;
		std::shared_ptr<const DX12::AccelerationSceneReady> scene;
		winrt::com_ptr<ID3D12Resource> camera;
		winrt::com_ptr<ID3D12Resource> lights;
		// Lifetime of diagnostic CBV is tied to the same DX12 GPU fence as the mask.
		winrt::com_ptr<ID3D12Resource> diagnostic;
		uint64_t geometryRevision = 0, lightRevision = 0;
		bool usedDiagnostic = false;
		bool updatedLightSpaceAtlas = false;
		uint64_t atlasSignature = 0;
	};

	std::string GetName() override { return "DX12 Native Composite"; }
	std::string GetShortName() override { return "DX12NativeComposite"; }
	std::string_view GetCategory() const override { return FeatureCategories::kDX12; }
	bool IsCore() const override { return true; }
	bool SupportsVR() override { return false; }
	// Automatic producer with user-selectable ray sampling and mask strength.
	// No separate enable or boot switch: DX12 master controls operation.
	bool IsAlwaysEnabled() const override { return true; }
	void DrawSettings() override;
	void LoadSettings(json& input) override;
	void SaveSettings(json& output) override;
	void RestoreDefaultSettings() override;
	bool WantsPreShadowMapsHook() const override { return true; }
	void PreShadowMaps() override;
	void OnRuntimeDisabled() override;
	void OnSceneTransitionReset(bool opening) override;
	bool IsActive() const;
	/** @brief Test-only suppression gate. Only fresh successful full-resolution DXR previews qualify. */
	bool ShouldDisableMatchedRasterShadows() const;
	std::optional<Submission> Dispatch(ID3D12GraphicsCommandList4* commands, ID3D12Resource* rasterColor,
		ID3D12Resource* depth, uint32_t width, uint32_t height);
	void OnSubmitted(ID3D12Fence* fence, uint64_t value, Submission submission, bool upscalerSucceeded);

private:
	struct PipelineState
	{
		winrt::com_ptr<ID3D12Device> device;
		winrt::com_ptr<ID3D12RootSignature> shadowRoot, lightSpaceRoot;
		winrt::com_ptr<ID3D12PipelineState> shadowPipeline, lightSpacePipeline;
		winrt::com_ptr<ID3D12RootSignature> diagnosticRoot;
		winrt::com_ptr<ID3D12PipelineState> diagnosticPipeline;
		uint32_t descriptorSize = 0;
	};
	struct UploadPair
	{
		winrt::com_ptr<ID3D12Resource> camera, lights, diagnostic;
	};
	static constexpr size_t kMaximumUploadPairs = 8;

	bool EnsurePipelines(ID3D12Device* device);
	std::shared_ptr<Surfaces> MakeSurfaces(ID3D12Resource* color, ID3D12Resource* depth, uint32_t width, uint32_t height);
	bool Upload(winrt::com_ptr<ID3D12Resource>& buffer, const void* source, size_t bytes, size_t capacity);
	D3D12_GPU_DESCRIPTOR_HANDLE Descriptor(const Surfaces& surfaces, uint32_t index) const;
	void Collect();

	struct Pending
	{
		winrt::com_ptr<ID3D12Fence> fence;
		uint64_t value = 0;
		Submission submission;
	};
	std::deque<Pending> pending;
	std::deque<UploadPair> availableUploads;
	size_t uploadPairs = 0;
	std::shared_ptr<Surfaces> surfaces;
	std::atomic<std::shared_ptr<const PipelineState>> preparedPipelines;
	std::atomic<bool> pipelineFailed = false;
	std::atomic<std::shared_ptr<const std::string>> pipelineFailureReason;
	std::atomic<std::shared_ptr<const std::string>> diagnosticFailureReason;
	std::atomic<bool> diagnosticUnavailable{ false };
	std::jthread pipelineWorker;
	winrt::com_ptr<ID3D12Device> device;
	winrt::com_ptr<ID3D12RootSignature> shadowRoot, lightSpaceRoot;
	winrt::com_ptr<ID3D12PipelineState> shadowPipeline, lightSpacePipeline;
	winrt::com_ptr<ID3D12RootSignature> diagnosticRoot;
	winrt::com_ptr<ID3D12PipelineState> diagnosticPipeline;
	uint32_t descriptorSize = 0;
	bool unavailable = false;
	// Sample only a fraction of pixels (no reconstruction). 0 = full,
	// 1 = 2/3, 2 = 1/2, 3 = 4/9, 4 = 1/3, 5 = 1/4, 6 = 1/6, 7 = 1/9.
	std::atomic<int> samplingMode{ 0 };
	std::atomic<float> shadowIntensity{ 1.0f };
	std::atomic<bool> diagnosticDarkenEnabled{ false };
	std::atomic<float> diagnosticDarkenStrength{ 1.0f };
	std::atomic<bool> disableMatchedRasterStatic{ false };
	// Classify using the companion's TES_LIGHT_FLAGS and live SCM movement state.
	std::atomic<bool> includeStationaryLights{ true };
	std::atomic<bool> includeFlickeringLights{ true };
	std::atomic<uint32_t> lastStationaryCount{ 0 }, lastFlickeringCount{ 0 };
	// A submitted light-space atlas, not a diagnostic color pass, arms test offload.
	std::atomic<uint32_t> lastNativeShadowFrame{ 0 };
	std::atomic<uint32_t> lastValidDiagnosticFrame{ 0 };
	bool previousRasterSuppressionArmed = false;
	uint64_t previousRasterRefreshRevision = 0;
	std::atomic<uint32_t> samplingPhase{ 0 };
	enum class DispatchStatus : uint8_t
	{
		NeverCalled,
		InactiveOrInputsMissing,
		PipelineNotReady,
		SceneNotReady,
		NoPreparedLights,
		SurfacesUnavailable,
		UploadBudgetExhausted,
		UploadFailed,
		Recorded,
		Submitted
	};
	std::atomic<DispatchStatus> lastDispatchStatus{ DispatchStatus::NeverCalled };
	std::atomic<uint64_t> dispatchCalls{ 0 };
	std::atomic<uint32_t> lastPreparedLightCount{ 0 };
	std::atomic<uint64_t> submittedMasks{ 0 };
	std::atomic<uint64_t> submittedDiagnosticFrames{ 0 };
};

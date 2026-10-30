#pragma once
#include "DenoiserAPI.h"
#include "GraphicsBackend.h"
#include "GraphicsInterop.h"
#include <FidelityFX/api/include/dx12/ffx_api_dx12.hpp>
#include <FidelityFX/api/include/ffx_api_loader.h>
#include <mutex>
#include <optional>

/** @brief Session-owned RR provider with an independent interop fence and bounded command pool. */
class RedstoneDenoiser
{
public:
	/** @brief Supplies separately denoised visibility textures and their enabled signal mask. */
	struct OcclusionResources
	{
		ID3D11Texture2D* diffuseInput;
		ID3D11Texture2D* specularInput;
		ID3D11Texture2D* diffuseOutput;
		ID3D11Texture2D* specularOutput;
		uint32_t signalFlags;
	};
	/** @brief Uses the shared loader API and loads the denoiser after backend eligibility passes. */
	bool Initialize(const GraphicsBackend& a_backend);
	~RedstoneDenoiser();
	bool CanDispatchRayRegeneration() const { return featureRayRegeneration && !rayRegenerationFailureLatched; }
	json GetRayRegenerationDiagnostics() const;
	/** @brief Returns saved overrides and provider defaults for the seven SDK configuration keys. */
	json GetConfiguration() const;
	/** @brief Replaces saved overrides; null entries select the provider defaults. */
	void SetConfiguration(const json& a_configuration);
	/** @brief Draws live SDK configuration controls. */
	void DrawConfigurationSettings();
	/** @brief Denoises the active RR grid within the captured engine rendering extent. */
	bool DispatchRayRegeneration(ID3D11Texture2D* a_signal, ID3D11Texture2D* a_linearDepth, ID3D11Texture2D* a_motionVectors,
		ID3D11Texture2D* a_normalRoughness, ID3D11Texture2D* a_specularAlbedo, ID3D11Texture2D* a_diffuseAlbedo,
		ID3D11Texture2D* a_output, ID3D11Texture2D* a_diffuseSignal, ID3D11Texture2D* a_diffuseOutput,
		const OcclusionResources& a_occlusion, const std::array<uint32_t, 2>& a_engineResolution, uint32_t a_width, uint32_t a_height, bool a_resetHistory,
		bool a_checkerboard, uint32_t a_checkerboardOrigin, bool a_directSignals = false);

private:
	bool ApplyConfiguration(bool& a_resetHistory);
	mutable std::mutex configurationMutex;
	std::array<std::optional<float>, 8> configurationOverrides{};
	std::array<float, 8> configurationDefaults{};
	bool configurationDefaultsReady = false;
	bool configurationDirty = true;
	uint64_t sharedConfigurationRevision = UINT64_MAX;
	void ResetRayRegeneration();
	bool EnsureRuntimeUpscalerInterop();
	struct RuntimeCommandContext
	{
		winrt::com_ptr<ID3D12CommandAllocator> commandAllocator;
		winrt::com_ptr<ID3D12GraphicsCommandList4> commandList;
		uint64_t fenceValue = 0;
		bool quarantined = false;
	};
	RuntimeCommandContext* AcquireRuntimeCommandContext();
	bool WaitForRuntimeD3D12Fence(uint64_t a_value) { return runtimeFence.CpuWait(a_value, SharedFence::kFenceTimeoutMs); }
	SharedFence runtimeFence;
	std::array<RuntimeCommandContext, 8> runtimeCommandContexts;
	uint32_t runtimeCommandContextCursor = 0;
	GraphicsBackend backend;
	HMODULE denoiserModule = nullptr;
	ffxFunctions denoiserFunctions{};
	bool featureRayRegeneration = false;
	bool rayRegenerationFailureLatched = false;
	ffxContext rayRegenerationContext = nullptr;
	uint64_t rayRegenerationSuccessfulDispatches = 0;
	uint64_t rayRegenerationFailedDispatches = 0;
	uint64_t rayRegenerationHistoryResets = 0;
	uint64_t rayRegenerationLastFenceValue = 0;
	uint32_t rayRegenerationWidth = 0;
	uint32_t rayRegenerationHeight = 0;
	std::array<uint32_t, 2> rayRegenerationMaxRenderSize{};
	bool rayRegenerationCheckerboard = false;
	bool rayRegenerationDirectSignals = false;
	WrappedResource* rrSharedSignal = nullptr;
	WrappedResource* rrSharedDepth = nullptr;
	WrappedResource* rrSharedMotion = nullptr;
	WrappedResource* rrSharedNormal = nullptr;
	WrappedResource* rrSharedSpecularAlbedo = nullptr;
	WrappedResource* rrSharedDiffuseAlbedo = nullptr;
	WrappedResource* rrSharedOutput = nullptr;
	WrappedResource* rrSharedDiffuseSignal = nullptr;
	WrappedResource* rrSharedDiffuseOutput = nullptr;
	std::array<WrappedResource*, 2> rrSharedOcclusionInput{};
	std::array<WrappedResource*, 2> rrSharedOcclusionOutput{};
	uint32_t rayRegenerationOcclusionSignals = 0;
	/** @brief Creates or reuses the paired indirect-lighting context and interop textures. */
	bool EnsureRayRegenerationResources(ID3D11Texture2D* a_signal, ID3D11Texture2D* a_linearDepth, ID3D11Texture2D* a_motionVectors,
		ID3D11Texture2D* a_normalRoughness, ID3D11Texture2D* a_specularAlbedo, ID3D11Texture2D* a_diffuseAlbedo,
		ID3D11Texture2D* a_output, ID3D11Texture2D* a_diffuseSignal, ID3D11Texture2D* a_diffuseOutput,
		const OcclusionResources& a_occlusion, const std::array<uint32_t, 2>& a_engineResolution, uint32_t a_width, uint32_t a_height, bool a_checkerboard, bool a_directSignals);
};

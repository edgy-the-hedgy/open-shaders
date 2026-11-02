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
    struct NativeHybridResources
    {
        // Layout: 0 diffuse, 1 specular, 2 AO, 3 specular occlusion.
        std::array<ID3D12Resource*, 4> inputs{}, outputs{};
        // Depth, motion, normal/roughness, diffuse albedo, specular albedo.
        std::array<ID3D12Resource*, 5> guides{};
        std::array<uint32_t, 2> nativeSize{}, logicalSize{};
        uint32_t frame = 0, checkerboardOrigin = 0;
        bool checkerboard = false, resetHistory = false;
        bool skipOcclusion = false; // deterministic AO/SO are not submitted to FSR-RR
    };
    // Records GPU-native RR into the caller's already-open DX12 command list.
    // Does not submit, signal fences, copy to DX11, or read back to CPU.
    bool DispatchNativeHybridRR(ID3D12GraphicsCommandList* list,
        const NativeHybridResources& resources);
    const std::string& NativeHybridFailure() const { return nativeHybridFailure; }
    uint64_t NativeHybridDispatches() const { return nativeHybridDispatches; }
    uint64_t NativeHybridResets() const { return nativeHybridResets; }
    /** @brief Safely retire only the native SDK contexts, preserving legacy RR. */
    bool RetireNativeHybridContexts();
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
    /** Applies the eight shared SDK keys to all three native contexts. */
    bool ApplyNativeHybridConfiguration(bool& a_resetHistory);
	mutable std::mutex configurationMutex;
	std::array<std::optional<float>, 8> configurationOverrides{};
	std::array<float, 8> configurationDefaults{};
	bool configurationDefaultsReady = false;
	bool configurationDirty = true;
	uint64_t sharedConfigurationRevision = UINT64_MAX;
    // Native contexts can report distinct AMD provider defaults. Never use
    // the legacy context's defaults as fabricated native configuration.
    std::array<std::array<float, 8>, 3> nativeHybridConfigurationDefaults{};
    std::array<bool, 3> nativeHybridConfigurationDefaultsReady{};
    std::array<bool, 3> nativeHybridConfigurationApplied{};
    uint64_t nativeHybridAppliedConfigurationRevision = UINT64_MAX;
    // SDK Query/Configure errors only block the rejected revision, allowing
    // the user to correct a value without reloading the renderer.
    uint64_t nativeHybridBlockedConfigurationRevision = UINT64_MAX;
    uint64_t nativeHybridConfigurationApplications = 0;
    std::string nativeHybridConfigurationFailure;
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
    std::array<ffxContext, 3> nativeHybridContexts{};
    std::array<uint32_t, 2> nativeHybridExtent{};
    bool nativeHybridCheckerboard = false;
    uint32_t nativeHybridLastFrame = UINT32_MAX;
    uint64_t nativeHybridDispatches = 0, nativeHybridResets = 0;
    bool nativeHybridFailed = false;
    std::string nativeHybridFailure;
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

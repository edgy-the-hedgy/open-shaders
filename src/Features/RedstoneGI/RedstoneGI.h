#pragma once
#include "Features/OverlayFeature.h"
#include "Buffer.h"
#include "RuntimeDenoiser.h"
#include "Utils/RayRegenerationSettings.h"
#include "StartupGate.h"
#include "Utils/LazyShader.h"


/** @brief Opt-in RGB lighting pipeline with independent controls and a private compositor. */
struct RedstoneGI final : OverlayFeature
{
	/** @brief Initializes independent settings without allocating a graphics backend. */
	RedstoneGI();
	static constexpr float kMinimumOccluderDepth = 1.0f;
	static constexpr float kMaximumOccluderDepth = 300.0f;
	static constexpr float kDefaultOccluderDepth = 300.0f;
	/** @brief Returns independently owned RGI defaults for feature-owned persistence. */
	static json DefaultRedstoneSettings();
	/** @brief Loads or saves RGI controls without reading or modifying baseline SSGI. */
	void LoadRedstoneSettings(const json& a_json);
	void SaveRedstoneSettings(json& a_json) const;
	void RestoreDefaultSettings() override;
	void DrawSettings() override;
	/** @brief Draws shared RR resolution and runtime preview controls. */
	void DrawRayRegenerationSettings();
	/** @brief Draws the independently saved vanilla SSAO control. */
	void DrawAmbientOcclusionSettings();
	void DrawPerformanceSettings() override {}
	void ApplyPerformanceProfile(PerfProfile) override {}
	bool MatchesPerformanceProfile(PerfProfile) const override { return true; }
	/** @brief Exposes only RGI settings; the registered feature owns persistence. */
	void LoadSettings(json& a_json) override;
	void SaveSettings(json& a_json) override;
	const void* GetSettingsBlob() const override;
	const void* GetBootValue(std::string_view a_key) const override;
	/** @brief Changes RGI sampling and invalidates reconstruction history. */
	void SetRRCheckerboard(bool a_enabled);
	bool IsRRCheckerboardEnabled() const { return RayRegeneration::GetSettings().checkerboard; }
	std::string GetDisplayName() override { return "RedstoneGI"; }
	std::string GetShortName() override { return "RedstoneGI"; }
	std::string GetName() override { return "RedstoneGI"; }
	std::string GetFeatureModLink() override { return {}; }
	bool UsesMainSettings() const override { return true; }
	bool IsAlwaysEnabled() const override { return true; }
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { "Independent Redstone global illumination with FSR Ray Regeneration. Select SSGI or RGI on this page.", {} };
	}
	/** @brief Reports whether the owned shaders and textures can render RGI. */
	bool IsRuntimeReady() const { return loaded && ShadersOK() && HasRayRegenerationResources(); }
	/** @brief Invalidates temporal outputs before activation or after a scene transition. */
	void ResetRuntimeHistory();
	void SetupResources() override;
	void ClearShaderCache() override;
	void PostPostLoad() override {}
	void OnSceneTransitionReset(bool) override { ResetRuntimeHistory(); }
	json GetDiagnostics() override;
	/** @brief Estimates independent RGB lighting and prepares current-frame RR or fallback output. */
	void DrawGI();
	enum class RRDebugSource : uint32_t
	{
		Off,
		Outputs,
		Inputs
	};
	/** @brief Selects mutually exclusive, unsaved RR input or output previews. */
	void SetRROutputOnly(bool a_enabled);
	void SetRRInputsOnly(bool a_enabled);
	RRDebugSource GetRRDebugSource() const { return rrDebugSource.load(std::memory_order_relaxed); }
	bool IsRROutputOnly() const { return GetRRDebugSource() == RRDebugSource::Outputs; }
	bool IsRRInputsOnly() const { return GetRRDebugSource() == RRDebugSource::Inputs; }
	bool IsRRDebugVisible() const { return GetRRDebugSource() != RRDebugSource::Off; }
	enum class RRDebugView : uint32_t
	{
		Combined = 0,
		Diffuse = 1,
		Specular = 2,
		DiffuseOcclusion = 3,
		SpecularOcclusion = 4,
		CombinedOcclusion = 5,
		AllSignals = 6,
		GeometryDepth = 7
	};
	/** @brief Selects a temporary signal or geometry preview without changing scene lighting or saved settings. */
	void SetRRDebugView(RRDebugView a_view) { rrDebugView.store(a_view, std::memory_order_relaxed); }
	RRDebugView GetRRDebugView() const { return rrDebugView.load(std::memory_order_relaxed); }
	/** @brief Draws runtime-only preview controls and its full-screen presentation overlay. */
	void DrawRRDebugSettings();
	void DrawRRDebugOverlay();
	/** @brief Binds the current paired lighting outputs and full-resolution composition guides. */
	void BindCompositeResources() override;
	/** @brief Retires private bindings and records whether the RGI compositor consumed RR. */
	void FinishComposite(bool a_usedPrivateShader) override;
	/** @brief Compiles the isolated compositor with the caller's existing feature permutations. */
	ID3D11ComputeShader* GetCompositeShader(bool a_interior, std::vector<std::pair<const char*, const char*>>& a_defines) override;

	/** @brief Preserves prior configuration when no RedstoneGI block exists. */
	void ImportLegacySettings(json& a_root) override;
	/** @brief Registers independently discoverable RGI commands and queries. */
	void RegisterUxActions() override;
	/** @brief Applies shader-setting changes before selecting the frame owner. */
	void OnWorldRenderBegin() override;
	/** @brief Opts in only when selection, session, resources and compositor permit RGI. */
	int GetIndirectLightingPriority() const override;
	bool UsesPrivateComposite() const override { return true; }
	void OnCompositeFailure() override { switchFailed = true; ResetRuntimeHistory(); }
	/** @brief Invalidates temporal state on every renderer ownership change. */
	void OnIndirectLightingSelection(bool a_selected) override;
	void DrawIndirectLighting() override { DrawGI(); }
	json GetRuntimeFlags() override;
	bool SetRuntimeFlag(std::string_view a_name, bool a_value) override;
	void DrawOverlay() override;
	bool IsOverlayVisible() const override;
	std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	/** @brief Chooses the renderer at the next deferred boundary. */
	void SelectRenderer(bool a_redstone) { selected = a_redstone; }
	/** @brief Reports feature-owned selection and backend eligibility. */
	json GetRedstoneStatus() const;
	/** @brief Provides saved provider overrides to the existing devbench commands. */
	RedstoneDenoiser& GetDenoiser() { return denoiser; }
	bool IsActive() const { return active.load(); }
	/** @brief Reports whether the selected provider has current-frame indirect lighting to compose. */
	bool HasCurrentIndirectLighting() const;
	/** @brief Changes normalized screen-space sampling work and invalidates reconstruction history. */
	void SetLightingSamples(uint32_t a_diffuseSlices, uint32_t a_specularSlices);
	/** @brief Returns diffuse and specular slice counts, each tracing both screen-space directions. */
	std::array<uint32_t, 2> GetLightingSamples() const { return { diffuseSlices, specularSlices }; }
	struct Settings
	{
		bool EnableVanillaSSAO = false;
		bool EnableAdaptiveSampling = true;
		float MinScreenRadius = 0.01f;
		float GIRadius = 4096.0f;
		float2 DepthFadeRange = { 4e4f, 5e4f };
		float GISaturation = 0.8f;
		float GIDistanceCompensation = 0.0f;
		float GIStrength = 1.0f;
	} settings;

private:
	bool selected = false;
	bool resourcesReady = false;
	bool switchFailed = false;
	std::atomic<bool> active{ false };
	std::optional<bool> previousVanillaSSAO;
	Redstone::StartupGate startupGate;
	void DrawRendererSettings();
	void QueueHistoryReset() { queuedResetHistory.store(true); }
	std::atomic<bool> queuedResetHistory{ true };
	bool recompileFlag = false;
	uint32_t rrResolutionMode = 1;
	eastl::unique_ptr<ConstantBuffer> settingsBuffer;
	eastl::unique_ptr<Texture2D> texNoise, texWorkingDepth, texRadiance;
	winrt::com_ptr<ID3D11SamplerState> linearClampSampler, pointClampSampler;
	winrt::com_ptr<ID3D11ComputeShader> prefilterDepthsCompute, giCompute;

	void EnsureRRDebugPreviewTexture(const std::array<uint, 2>& a_size);
	void PrepareGeometryDebugPreview(const std::array<uint, 2>& a_size);
	eastl::unique_ptr<StructuredBuffer> geometryDepthMaximum;
	eastl::unique_ptr<ConstantBuffer> geometryPreviewSettings;
	Util::LazyShader<ID3D11ComputeShader> geometryDepthReduceShader;
	Util::LazyShader<ID3D11ComputeShader> geometryDepthPreviewShader;
	void SetupCompositeResources();
	void EnforceFixedPath();
	bool rrEnabled = true;
	bool diffuseEnabled = true;
	bool specularEnabled = true;
	bool rrCheckerboardEnabled = true;
	uint64_t rrPolicyRevision = UINT64_MAX;
	bool rrCheckerboardThisFrame = false;
	uint32_t rrCheckerboardOrigin = 0;
	float rgiContributionEV = 0.0f;
	float specularContributionEV = 0.0f;
	float diffuseContributionEV = 0.0f;
	uint diffuseSlices = 2;
	uint diffuseSteps = 10;
	uint specularSlices = 2;
	uint specularSteps = 10;
	float occluderDepth = kDefaultOccluderDepth;
	float giCompensationMaxDistance = 256.0f;
	float specularRoughnessFloor = 0.2f;
	struct OcclusionSettings
	{
		uint diffuseEnabled = 0, specularEnabled = 0;
		uint diffuseSlices = 2, specularSlices = 2;
		uint diffuseSteps = 10, specularSteps = 10;
		float diffuseRadius = 256.0f, specularRadius = 256.0f;
		float diffuseStrength = 1.0f, specularStrength = 1.0f;
		uint useDenoised = 0, padding = 0;
	} occlusion;
	STATIC_ASSERT_ALIGNAS_16(OcclusionSettings);
	eastl::unique_ptr<ConstantBuffer> occlusionSettings;
	eastl::unique_ptr<Texture2D> texDiffuseOcclusionInput, texSpecularOcclusionInput;
	eastl::unique_ptr<Texture2D> texDiffuseOcclusionOutput, texSpecularOcclusionOutput, texPackedOcclusion;
	winrt::com_ptr<ID3D11ComputeShader> occlusionCompute, occlusionPackCompute;
	void DrawOcclusion(const std::array<uint, 2>& a_size);
	void PackOcclusion(const std::array<uint, 2>& a_size, bool a_denoised);
	bool rrLightingReady = false;
	bool rrProviderSucceeded = false;
	eastl::unique_ptr<ConstantBuffer> compositionSettings;
	winrt::com_ptr<ID3D11Buffer> previousCompositeSettings;
	Util::LazyShader<ID3D11ComputeShader> compositeShader;
	Util::LazyShader<ID3D11ComputeShader> compositeInteriorShader;
	uint32_t GetSignalMask() const { return (diffuseEnabled ? 1u : 0u) | (specularEnabled ? 2u : 0u) | (occlusion.diffuseEnabled ? 4u : 0u) | (occlusion.specularEnabled ? 8u : 0u); }
	void PrepareRRDebugPreview(const std::array<uint, 2>& a_size);
	bool IsRRDebugPreviewCurrent() const;
	std::atomic<RRDebugSource> rrDebugSource{ RRDebugSource::Off };
	RRDebugSource rrPreviewSource = RRDebugSource::Off;
	uint32_t rrPreviewSignalMask = 0;
	bool rrPreviewBypassed = false;
	std::atomic<RRDebugView> rrDebugView{ RRDebugView::Combined };
	RRDebugView rrPreviewView = RRDebugView::Combined;
	std::atomic<float> rrPreviewExposureEV{ 0.0f };
	bool rrPreviewValid = false;
	bool rrPreviewFailed = false;
	uint32_t rrPreviewGameFrame = 0;
	uint32_t rrPreviewSequence = 0;
	std::array<uint, 2> rrPreviewSize{};
	eastl::unique_ptr<Texture2D> rrPreview;
	eastl::unique_ptr<ConstantBuffer> rrPreviewSettings;
	Util::LazyShader<ID3D11ComputeShader> rrPreviewShader;
	void CompileComputeShaders();
	bool ShadersOK() const;
	void UpdateSB();
	void PrepareSurfaceInputs(const std::array<uint, 2>& a_resolution, const std::array<uint, 2>& a_internalResolution);
	void DrawEstimators(const std::array<uint, 2>& a_internalResolution);
	const char* GetRayRegenerationInactiveReason() const;
	bool HasRayRegenerationResources() const;
	void SetupRayRegenerationResources(const D3D11_TEXTURE2D_DESC& a_textureDesc);
	void DrawRayRegeneration(const std::array<uint, 2>& a_engineResolution, const std::array<uint, 2>& a_internalResolution);
	struct alignas(16) RGICB
	{
		float2 NDCToViewMul;
		float2 NDCToViewAdd;
		float2 TexDim;
		float2 RcpTexDim;
		float2 FrameDim;
		float2 RcpFrameDim;
		uint FrameIndex;
		uint DiffuseSlices;
		uint DiffuseSteps;
		uint SpecularSlices;
		uint SpecularSteps;
		uint RRHistoryValid;
		uint RRCheckerboardEnabled;
		uint RRCheckerboardOrigin;
		float MinScreenRadius;
		float GIRadius;
		float EffectRadius;
		float SpecularRoughnessFloor;
		float2 DepthFadeRange;
		float GISaturation;
		float GIDistanceCompensation;
		float GICompensationMaxDist;
		float GIStrength;
		float DepthFadeScaleConst;
		float OccluderDepth;
		Matrix PreviousCameraView;
		Matrix PreviousCameraProjection;
		float4 PreviousCameraOrigin;
		uint GeometryHistoryValid;
		uint geometryPadding[3]{};
	};
	STATIC_ASSERT_ALIGNAS_16(RGICB);
	RedstoneDenoiser denoiser;
	eastl::unique_ptr<Texture2D> texRRSpecularInput = nullptr;
	eastl::unique_ptr<Texture2D> texRRSpecularOutput = nullptr;
	eastl::unique_ptr<Texture2D> texRRDiffuseInput = nullptr;
	eastl::unique_ptr<Texture2D> texRRDiffuseOutput = nullptr;

	eastl::unique_ptr<Texture2D> texRRLinearDepth;
	bool rrHistoryValid = false;
	bool geometryHistoryValid = false;
	Matrix previousCameraView{};
	Matrix previousCameraProjection{};
	float4 previousCameraOrigin{};
	bool rrOutputApplied = false;
	uint32_t rrOutputFrame = UINT32_MAX;
	std::array<uint, 2> rrHistoryResolution{};
	eastl::unique_ptr<Texture2D> texRRMotionVectors = nullptr;
	eastl::unique_ptr<Texture2D> texRRNormalRoughness = nullptr;
	eastl::unique_ptr<Texture2D> texRRSpecularAlbedo = nullptr;
	eastl::unique_ptr<Texture2D> texRRDiffuseAlbedo = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> rrPrepareInputsCompute;
	winrt::com_ptr<ID3D11ComputeShader> rrResolveFallbackCompute;
	winrt::com_ptr<ID3D11ComputeShader> sourceRadianceCompute;
};

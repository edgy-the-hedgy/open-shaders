#pragma once

#include "../OverlayFeature.h"
#include "DX12NativeAcceleration.h"
#include "DX12NativeLightWorker.h"
#include "DX12NativeRasterCapture.h"
#include "DX12NativeWorldFrame.h"
#include "DX12ProjectedInputs.h"
#include "DX12NativeCharacterCasters.h"
#include "Features/NativePBR/NativeMaterialMetadata.h"
#include "DX12NativeScreenProbe.h"
#include "DX12NativeDirectLighting.h"
#include "DX12NativeIndirectLighting.h"
#include "DX12NativeRuntimeCapture.h"
#include "DX12NativeShadowTrace.h"
#include "SCMNativePolicy.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/** @brief Independent early opaque raster capture for native DX12. */
struct DX12NativeScene final : OverlayFeature
{
	std::string GetName() override { return "DX12 Native Scene"; }
	std::string GetShortName() override { return "DX12NativeScene"; }
	std::string_view GetCategory() const override { return FeatureCategories::kDX12; }
	bool IsCore() const override { return true; }
	bool IsAlwaysEnabled() const override { return true; }
	bool IsInMenu() const override { return false; }
	bool IsInFeatureList() const override { return false; }
	bool SupportsVR() override { return false; }

	void DrawSettings() override;
	void DrawDevSettings();
	void DrawLightConsolidationSettings();
	void RegisterUxActions() override;
	json GetStatus() const;
	bool CasterInTlas(RE::BSGeometry& geometry) const;
	void AuditCasterCoverage(RE::BSGeometry& geometry) const;
	bool WantsExactShadowOrigins() const { return frameExactShadowLights.load(std::memory_order_relaxed); }
	uint32_t GetFullNativeQuality(uint32_t frame) const { return fullNativeQualityFrame == frame ? fullNativeFrameQuality : DX12Native::SanitizeRTQuality(fullRTQuality); }
	uint32_t GetLightObservationFrames() const { return lightObservationFrames.load(std::memory_order_relaxed); }
	DX12Native::NativeLightSettings GetFullNativeLightSettings() const
	{
		return { runtimeUnificationRadius.load(std::memory_order_relaxed),
			preparedExclusionRadius.load(std::memory_order_relaxed),
			additivePreparedEmission.load(std::memory_order_relaxed), false };
	}
	void ObserveScheduledShadowOrigin(const RE::NiLight* light, const RE::NiPoint3& origin);
	std::shared_ptr<const DX12Native::TopLevel> GetPublishedScene() const { return acceleration.GetScene(); }
	/** @brief The selected mode's exterior preparation and shadow contribution setting. */
	bool AllowsOutdoorContribution() const;
	/** @brief Comparison never admits native scene work, even for interiors. */
	bool AllowsNativeSceneWork() const;
	bool AllowsScenePreparation(bool exterior) const { return AllowsNativeSceneWork() && (!exterior || AllowsOutdoorContribution()); }
	void DrawOverlay() override;
	bool IsOverlayVisible() const override;
	bool WantsRenderPassHook() const override;
	std::function<void()> OnRenderPassBegin(const RE::BSRenderPass* pass) override;
	bool WantsPreShadowMapsHook() const override;
	void PreShadowMaps() override;
	/** @brief Prepares a loading destination through the same acceleration worker without scene ownership. */
	void PrepareLoadingScene(std::shared_ptr<const DX12::Grid> grid);
	/** @brief Retires a whole-cell build when it cannot fit the current DXGI budget. */
	bool RetireBudgetDeferredWholeCell(uint64_t revision)
	{
		if (!acceleration.WholeCellBudgetDeferred(revision))
			return false;
		acceleration.Reset();
		shadowLightScope.store(0, std::memory_order_release);
		shadowLights.store(nullptr, std::memory_order_release);
		return true;
	}
	void LoadSettings(json& a_input) override;
	void SaveSettings(json& a_output) override;
	void RestoreDefaultSettings() override;
	bool WantsOpaqueRasterReadyHook() const override;
	void OnOpaqueRasterReady() override;
	void ObserveWorldCamera(const globals::FrameBuffer& camera);
	const GraphicsBackend* GetNativeBackend() const { return backend ? &*backend : nullptr; }
	std::shared_ptr<const DX12Native::NativeWorldFrame> GetWorldFrame(uint32_t engineFrame, uint32_t pluginFrame) const;
	void BeginFullNativeRasterInputs();
	void StampFullNativeIndexedDraw(uint32_t count, uint32_t start, int32_t base, bool indexed = true, uint32_t instances = 1, uint32_t firstInstance = 0, bool gpuCounted = false, bool automatic = false);
	void ObserveNativeInputTargets(UINT count, ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth);
	void ObserveNativeInputPS(ID3D11PixelShader* ps) { nativeInputPS = ps; }
	void ObserveNativeInputBlend(ID3D11BlendState* state, UINT sampleMask);
	void ObserveNativeInputDepth(ID3D11DepthStencilState* state);
	void ObserveNativeInputTopology(D3D11_PRIMITIVE_TOPOLOGY topology) { nativeInputTopology = topology; }
	void ObserveNativeInputIndexOffset(DXGI_FORMAT format, UINT offset) { nativeInputIndexOffset = offset; nativeInputIndexElementBytes = format == DXGI_FORMAT_R16_UINT ? 2u : format == DXGI_FORMAT_R32_UINT ? 4u : 0u; }
	void InvalidateNativeInputVisibility();
	void ObserveNativeInputClear(ID3D11View* view);
	void ObserveNativeInputSourceViews(UINT start, UINT count, ID3D11ShaderResourceView* const* views);
	void ResetNativeInputBindingState();
	std::shared_ptr<const NativePBR::RawInputFrame> GetRawInputs(uint32_t frame) const { return rawInputCapture.GetForFrame(frame); }
	// Capture the completed fallback radiance before native final-color application.
	void CaptureFullNativeEnvironment(ID3D11ShaderResourceView* sceneColor, uint32_t frame);
	bool WantsDepthPrepassReadyHook() const override;
	void OnDepthPrepassReady() override;
	/** @brief Observe CPU-backed opaque rigid geometry from SCM or normal render passes. */
	void ObserveSCMRigidCaster(RE::BSGeometry& geometry, bool requireStability = false);
	/** @brief Snapshot engine-selected projection views after Lighting SetupGeometry; never read unmapped constants. */
	/** @brief SCM hands native-owned lights' static casters to RayQuery and retains movers. */
	bool OwnsStaticForLight(RE::BSShadowLight* light, int32_t slot) const;
	/** @brief Frame-wide policy established before SCM scheduling. */
	DX12::SCMNativeMode GetFrameNativeMode() const;
	// Snapshot LLF/SCM point-light parameters without altering the DX11 light.
	void ObserveRuntimeLight(RE::BSShadowLight* light, uint32_t referenceID, bool flickers,
		const RE::NiPoint3& restOrigin);
	void OnLoadingMenuEvent(bool opening) override;
	void OnSceneTransitionReset(bool a_opening) override;
	void OnRuntimeDisabled() override;

	/** @brief Raw mode-sized shadow output, independently fenced for Native Composite. */
	std::shared_ptr<const DX12Native::ShadowMaskFrame> GetShadowMask(uint32_t frame) const { return shadows.Get(frame); }
	std::shared_ptr<const DX12Native::NativeScreenProbe::MaterialFrame> GetFullNativeMaterial(uint32_t frame) const;

	std::shared_ptr<const DX12Native::DirectLightingFrame> GetFullNativeDirect(uint32_t frame) const
	{
		return fullNativeDirect ? fullNativeDirect->Get(frame) : nullptr;
	}

	std::shared_ptr<const DX12Native::IndirectLightingFrame> GetFullNativeIndirect(uint32_t frame) const;

private:
	/** @brief Gates native scene work; never disables DX11 baseline shadows. */
	bool CanOffloadShadowsInCurrentCell() const;
	bool nativeSessionStarted = false;  // Render/main-thread lifecycle.
	std::atomic<bool> loadingResetPending{ false };
	void ApplyPendingLoadingReset();
	std::optional<GraphicsBackend> backend;
	DX12Native::RasterCapture raster;
	std::atomic<std::shared_ptr<const DX12Native::NativeWorldFrame>> worldFrame;
	uint64_t rejectedWorldCameraUploads = 0;
	void DispatchFullNativeMaterial(bool opaqueReady = false);
	void ObserveFullNativeRasterDraw(RE::BSGeometry& geometry);
	void FinishFullNativeRasterDraws();
	// D3D11-only GPU snapshots, never CPU-mapped. Reused in immediate-context
	// submission order; not exported to DX12 until a fenced publication exists.
	struct ProjectedConstantSlot
	{
		std::array<winrt::com_ptr<ID3D11Buffer>, 2> buffers;
		std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> views;
		std::array<uint32_t, 2> byteWidths{};
	};
	static constexpr uint32_t kMaxProjectedConstantSnapshots = DX12Native::kMaxProjectedDraws;
	static constexpr uint32_t kTerrainSnapshotCapacity=32u;
	static constexpr uint32_t kOtherSnapshotCapacity=kMaxProjectedConstantSnapshots-kTerrainSnapshotCapacity;
	std::unique_ptr<DX12Native::NativeCharacterCasters> characterCasters;
	std::array<ProjectedConstantSlot, kMaxProjectedConstantSnapshots> projectedConstantPool;
	winrt::com_ptr<ID3D11Device> projectedConstantDevice;
	struct ProjectedTextureObservation
	{
		// Lighting.hlsl projected diffuse, normal, detail, and noise SRVs.
		std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 4> views;
		bool observed = false, conflicted = false;
		uint32_t descriptor = 0, samplerModes = 0;
		uint32_t constantSlot = UINT32_MAX;
		uint32_t rowKind=UINT32_MAX, extraData=0;
		std::array<uint32_t, 4> constantOffsets{};
		bool repeatedConstants = false;
	};
	bool CaptureProjectedConstants(ProjectedTextureObservation& observation);
	void CaptureRasterProjectedDraw(uint32_t rasterDraw, uint32_t sceneInstance);
	void CaptureRasterPbrGrass(uint32_t rasterDraw);
	void CaptureRasterTerrainDraw(uint32_t rasterDraw,uint32_t descriptor);
	struct RasterProjectionObservation
	{
		uint32_t rasterDraw = 0, sceneInstance = UINT32_MAX;
		ProjectedTextureObservation texture;
	};
	// Render-thread-only observations, deliberately separate from SCM ownership.
	struct FullNativeRasterDrawState
	{
		std::shared_ptr<const DX12Native::TopLevel> scene;
		DX12Native::MaterialFrameContract::RasterIdentityStamp stamp{};
		// Epoch-stamped observation rows: no scene-sized clear on normal frames.
		std::vector<uint32_t> seen;
		uint32_t seenEpoch = 0;
		struct CasterMatchEntry
		{
			DX12::CasterSource source{};
			std::array<float, 12> transform{};
			DX12Native::MaterialFrameContract::UniqueRasterCandidate match{};
		};
		// Bounded direct-mapped cache scoped to the immutable SceneInputs publication.
		static constexpr size_t kCasterMatchSlots = 8192;
		std::vector<CasterMatchEntry> casterMatches;
		uint32_t matchHits = 0, matchMisses = 0;
		std::unordered_map<const void*, uint32_t> geometryOrdinals, materialOrdinals;
		std::vector<NativePBR::RasterDrawRecord> drawRecords;
		std::vector<std::shared_ptr<const NativePBR::RasterSourceViews>> drawSourceViews;
		std::vector<std::shared_ptr<const DX12::PreparedMaterialSource>> materialSources;
		uint32_t materialSnapshotHits = 0, materialSnapshotMisses = 0;
		uint32_t staticOwnerBypasses = 0;
		uint32_t activeGeometry = 0, activeMaterial = 0, nextRasterDraw = 0;
		uint32_t activeSceneInstance = 0, activePrimitiveCount = 0;
		// Sparse, frame-local observations indexed by the matched TLAS instance.
		std::unordered_map<uint32_t, DX12Native::MaterialFrameContract::ProjectedPropertyObservation> projection;
		std::unordered_map<uint32_t, ProjectedTextureObservation> projectionTextures;
		std::vector<RasterProjectionObservation> rasterProjectionTextures;
		uint32_t activeProjectedInstance = UINT32_MAX;
		uint32_t constantSnapshots = 0, constantSnapshotFailures = 0;
		uint32_t grassConstantSnapshots=0, terrainConstantSnapshots=0, terrainCandidates=0;
		std::vector<std::pair<uint32_t,uint32_t>> grassConstants;
		uint32_t projectedDraws = 0, unmatchedProjectedDraws = 0;
		uint32_t draws = 0, candidates = 0, uniqueInstances = 0, ambiguous = 0, rejected = 0;
		bool finalized = false, active = false, allWritersKnown = true;
		uint32_t knownWriters = 0, unknownWriters = 0;
		bool targetsBound = false, identityTargetsBound = false, touchesVisibility = true, replacement = false, opaqueReplacement = false, depthTest = false;
	};
	ID3D11PixelShader* nativeInputPS = nullptr;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, NativePBR::RasterSourceViews::kTrackedSlots> nativeSourceViews;
	std::shared_ptr<const NativePBR::RasterSourceViews> nativeSourceSnapshot;
	bool nativeSourceViewsDirty = true;
	std::unordered_map<ID3D11ShaderResourceView*,DX12Native::TextureRevision::ViewStamp> nativeSourceRevisionCache;
	uint64_t nativeSourceRevisionEpoch=0,nativeSourceRevisionSerial=0;
	uint32_t nativeMaterialTransferSequence = 0;
	uint32_t terrainSnapshotStart=0, terrainSnapshotScope=0;
	D3D11_PRIMITIVE_TOPOLOGY nativeInputTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	UINT nativeInputIndexOffset = UINT_MAX, nativeInputIndexElementBytes = 0;
	FullNativeRasterDrawState fullNativeRasterDraws;
	struct RasterMaterialCacheEntry
	{
		uintptr_t property = 0, material = 0;
		DX12::PreparedMaterialSource scalars;
		std::shared_ptr<const NativePBR::TextureSetSnapshot> textures;
		std::shared_ptr<const DX12::PreparedMaterialSource> snapshot;
	};
	std::vector<RasterMaterialCacheEntry> rasterMaterialCache;
	std::shared_ptr<const DX12::PreparedMaterialSource> ObserveRasterMaterial(RE::BSGeometry& geometry);
	NativePBR::RawInputCapture rawInputCapture;
	std::unique_ptr<DX12Native::DirectLightingTrace> fullNativeDirect;
	void DispatchFullNativeIndirect();
	void ResetFullNativeEnvironment();
	DX12Native::RuntimeGeometryCapture geometryCapture;
	DX12Native::AccelerationWorker acceleration;
	DX12Native::ShadowMaskTrace shadows;
	// One immutable, ordered light selection shared by SCM and the RayQuery dispatch.
	std::atomic<std::shared_ptr<const std::vector<DX12::PreparedLight>>> shadowLights;
	std::atomic<uint32_t> shadowLightScope{ 0 };
	std::atomic<DX12::SCMNativeMode> frameNativeMode{ DX12::SCMNativeMode::Disabled };
	std::atomic<uint32_t> frameNativeModeID{ UINT32_MAX };
	std::atomic<bool> outdoorShadowOffloading{ false };  // Off by default; vanilla raster shadows are unaffected.
	std::atomic<bool> outdoorFullRT{ false };
	std::atomic<bool> debugOffloadLights{ false };
	std::atomic<bool> debugRTLights{ false };
	bool WantsLightPreview(DX12::SCMNativeMode mode) const;
	uint32_t fullRTQuality = DX12Native::kShadowQualityQuality;
	uint32_t characterUpdateInterval = 1u;
	uint32_t fullNativeQualityFrame = UINT32_MAX, fullNativeFrameQuality = DX12Native::kShadowQualityQuality;
	uint32_t FullNativeFrameQuality(uint32_t frame)
	{
		if (fullNativeQualityFrame != frame) {
			fullNativeQualityFrame = frame;
			fullNativeFrameQuality = DX12Native::SanitizeRTQuality(fullRTQuality);
		}
		return fullNativeFrameQuality;
	}
	std::atomic<uint32_t> runtimeUnificationRadius{ DX12Native::kDefaultRuntimeUnificationRadius };
	std::atomic<uint32_t> preparedExclusionRadius{ DX12Native::kDefaultPreparedExclusionRadius };
	std::atomic<bool> additivePreparedEmission{ true };
	std::atomic<uint32_t> lightObservationFrames{ DX12Native::kDefaultLightObservationFrames };
	std::shared_ptr<const DX12Native::NativeLightPublication> CollectFullRTLights(uint32_t frame);
	bool ObserveFullRuntimeLights();
	DX12Native::NativeLightWorker lightWorker;
	std::shared_ptr<const DX12::Grid> fullRequestedGrid;
	std::shared_ptr<const std::vector<DX12Native::NativeRuntimeEmitter>> fullRuntimeSnapshot, fullRequestedRuntime;
	DX12Native::NativeLightSettings fullRequestedSettings;
	uint32_t fullRuntimeCell = 0, fullLastObservation = 0, fullLastRequestFrame = 0;
	bool fullObserved = false;
	void ResetFullLightInventory();
	struct RuntimeLightSample
	{
		const RE::NiLight* source = nullptr;  // Identity of the SCM light actually observed.
		uint32_t referenceID = 0;             // Corresponding prepared native-scene emitter.
		DX12::PreparedLight value;
	};
	std::atomic<bool> coverageAudit{ false };
	std::atomic<bool> earlyTraceExperiment{ false };
	std::atomic<bool> lightVisibilityExperiment{ false };
	std::atomic<bool> exactShadowLights{ false }, frameExactShadowLights{ false };
	std::unordered_map<uintptr_t, float3> scheduledOrigins;
	uint32_t scheduledOriginsFrame = UINT32_MAX;
	std::vector<DX12::PreparedLight> lightsAtShadowOrigins;
	std::atomic<uint32_t> movingOriginCount{ 0 };
	mutable std::atomic<uint64_t> coveredCasterChecks{ 0 }, missingCasterChecks{ 0 };
	mutable std::atomic<uint64_t> ownershipGranted{ 0 };
	mutable std::mutex runtimeLightMutex;
	uint32_t runtimeLightFrame = UINT32_MAX;
	std::vector<RuntimeLightSample> runtimeLightSamples;
	// Full-shadow membership is independent of SCM selection. Keep emitter identity alive
	// only while the source belongs to the active world light lists.
	struct FullRuntimeLight
	{
		RE::NiPointer<RE::NiLight> source;
		DX12::PreparedLight value;
		uint32_t placedReference = 0;
		uint64_t seen = 0;
		bool supported = false, sampled = false, active = false, shadowLight = false;
	};
	std::unordered_map<const RE::NiLight*, FullRuntimeLight> fullRuntimeLights;
	uint32_t fullRuntimeScope = 0;
	uint64_t fullLightVisit = 0;
};

#pragma once

#include "AccelerationScene.h"
#include "Buffer.h"
#include "Features/LightLimitFix/LightView.h"
#include "MaterialResources.h"
#include "PreparedScene.h"
#include "Features/RedstoneGI/RuntimeDenoiser.h"
#include "NativeHybridGI.h"
#include <atomic>
#include <span>

namespace DX12Native
{
	/** @brief Traces directional blocker distances and publishes an explicitly partial raw preview. */
	class DirectionalTrace
	{
	public:
		static constexpr uint32_t kFirstDirectView = 10;
		static constexpr uint32_t kCombinedDirectView = 12;
		static constexpr uint32_t kMaximumDirectLights = 32;
		// Persisted sampling modes: 0=full, 1=half, 2=quarter, 3=two-thirds, 4=one-third.
		// The menu lists them by decreasing quality without renumbering saved settings.
		static constexpr std::array<uint32_t, 5> kTraceResolutionMenuOrder{ 0u, 3u, 1u, 4u, 2u };
		static constexpr const char* kTraceResolutionMenuLabels = "Full\0Two-thirds\0Half\0One-third\0Quarter\0";
		/** @brief Loads packaged DXIL using the existing device; compiles only native receiver/preview passes. */
		bool Initialize(const GraphicsBackend& a_backend);
		/** @brief Preserves in-flight ray resources during early teardown. */
		~DirectionalTrace();
		/** @brief Invalidates publication while pending frames retire by fence. */
		void Reset(bool a_releaseBindings = false);
		/** @brief Retries shader loading after an explicit cache reload. */
		void ReloadShaders();
		/** @brief Retires frames and consumes completed raw output without CPU waits. */
		void Retire();
		/** @brief Traces selected-light visibility or a bounded prepared light set against a retained TLAS. */
		void Dispatch(std::shared_ptr<const TopLevel> a_scene, const std::array<uint32_t, 2>& a_nativeSize, const float3& a_lightDirection, uint32_t a_frame, uint64_t a_budgetBytes, bool a_localLight = false, const LightLimitFixData::LightView& a_lights = {}, uint32_t a_materialView = 0, ID3D11UnorderedAccessView* a_sceneTarget = nullptr, uint32_t a_lightReference = 0, uint32_t a_lightBase = 0, ID3D11ShaderResourceView* a_roomMetadata = nullptr, std::span<const std::array<uint32_t, 2>> a_lightIdentities = {}, bool a_allPreparedLights = false, float4 a_strengths = { 1, 1, 1, 1 }, bool a_useIndirect = true, float a_indirectStrength = 1.0f, bool a_useDirectRR = false, uint32_t a_indirectRays = 0, std::span<const uint32_t> a_roomReferences = {}, float a_rasterBlend = 1.0f, uint32_t a_normalMode = 2u, std::span<const PreparedPointLight> a_preparedLights = {}, uint32_t a_directResolutionMode = 0u, uint32_t a_indirectResolutionMode = 0u, uint32_t a_directRRResolutionMode = 3u, bool a_rrMaterialIsolation = false, float a_rrGlossyRoughness = 0.2f, float4 a_falloffParameters = { 1.0f, 0.1f, 1.0f, 0.0f }, float4 a_stochasticParameters = { 0.0f, 1.0f, 0.0f, 0.0f });
		/** @brief Non-owning, fence-gated DX12 mask publication. Consumers must wait on completionFence/value
		 * and use the resources before the owning frame is retired; no CPU readback is involved. */
		struct ShadowMaskPublication
		{
			ID3D12Resource* mask = nullptr;
			ID3D12Resource* perLightVisibility = nullptr;
			ID3D12Fence* completionFence = nullptr;
			uint64_t completionValue = 0;
			std::array<uint32_t, 2> size{};
			explicit operator bool() const { return mask && perLightVisibility && completionFence && completionValue; }
		};
		/** @brief Returns a current, generation-matched mask without consuming it on D3D11. */
		ShadowMaskPublication GetShadowMask(uint32_t a_frame) const;
        // Stage 4A: GPU-only, frame-fence-gated DX11->DX12 input transport.
        // Channels: scene color, depth, normal/roughness, albedo, reflectance,
        // motion vectors. Depth is RGBA32F to preserve reconstruction precision;
        // the other five inputs use RGBA16F.
        static constexpr uint32_t kHybridInputCount = 6;
        struct HybridInputsPublication
        {
            std::array<ID3D12Resource*, kHybridInputCount> inputs{};
            ID3D12Fence* completionFence = nullptr;
            uint64_t completionValue = 0;
            std::array<uint32_t, 2> size{};
            explicit operator bool() const { return inputs[0] && completionFence && completionValue; }
        };
        HybridInputsPublication GetHybridInputs(uint32_t a_frame) const;
        // The scene texture is *linear radiance*, not gamma-encoded raster.
        struct ShadowedSourcePublication {
            ID3D12Resource* scene = nullptr;
            ID3D12Resource* linearDepth = nullptr;
            ID3D12Fence* completionFence = nullptr;
            uint64_t completionValue = 0;
            std::array<uint32_t, 2> size{};
            explicit operator bool() const { return scene && completionFence && completionValue; }
        };
        ShadowedSourcePublication GetShadowedSource(uint32_t a_frame) const;
        struct NativeGIOutputsPublication
        {
            std::array<ID3D12Resource*, NativeHybridGI::kOutputs> outputs{};
            std::array<ID3D12Resource*, NativeHybridGI::FrameOutputs::kGuideCount> rrGuides{};
            bool rrGuidesValid = false;
            bool rrValid = false, postValid = false;
            std::array<ID3D12Resource*, NativeHybridGI::FrameOutputs::kSignals> rrOutputs{};
            std::array<ID3D12Resource*, NativeHybridGI::FrameOutputs::kSignals> reconstructed{};
            HybridSamplingLayout sampling{};
            // Optional DX12-composed final scene. Valid only for the same frame/fence.
            ID3D12Resource* composedScene = nullptr;
            bool composedValid = false;
            ID3D12Fence* completionFence = nullptr;
            uint64_t completionValue = 0;
            std::array<uint32_t, 2> size{};
            explicit operator bool() const { return outputs[0] && outputs[1] && completionFence && completionValue; }
        };
        NativeGIOutputsPublication GetNativeGIOutputs(uint32_t a_frame) const;
        // Non-owning diagnostic transport publication. The -2 alpha sentinel
        // identifies an UNSAMPLED family; -1 identifies a sampled miss.
        // Never treat one family's validity as evidence for the other.
        // Consumers must wait on completionFence and finish before frame retirement.
        struct TransportSignalsPublication
        {
            ID3D12Resource* diffuse = nullptr;
            ID3D12Resource* specular = nullptr;
            ID3D12Fence* completionFence = nullptr;
            uint64_t completionValue = 0;
            uint32_t frameNumber = 0;
            uint32_t tier = 0, traceMode = 0;
            std::array<uint32_t, 2> size{};
            explicit operator bool() const
            {
                return diffuse && specular && completionFence && completionValue &&
                    tier >= 2u && tier <= 5u;
            }
        };
        TransportSignalsPublication GetTransportSignals(uint32_t a_frame) const;
		/** @brief Returns a raw preview view whose image remains owned by this service. */
		ID3D11ShaderResourceView* Preview() const { return previewValid && preview ? preview->srv.get() : nullptr; }
		/** @brief Returns the completed preview dimensions for an undistorted diagnostic image. */
		std::array<uint32_t, 2> PreviewSize() const { return previewSize; }
		/** @brief Records an ImGui preview draw reference separately from scene-lighting consumption. */
		void MarkPreviewDrawSubmitted() { if (Preview()) ++previewDrawSubmissions; }
		/** @brief Reports raw traversal and preview evidence without claiming RR or replacement coverage. */
		json GetDiagnostics() const;
        // Existing trace-owned allocations, excluding separate material pools.
        // Auto-budget adds only LOCAL DXGI headroom to this retained amount.
        uint64_t TrackedResidentBytes() const { return ResidentBytes(); }
		/** @brief Draws prepared RR context diagnostics and current input/output buffers. */
		void DrawRayRegenerationSettings(bool a_dxrIndirect);
		/** @brief Presents an explicitly selected prepared-signal RR debug overlay. */
		void DrawRRDebugOverlay();
		/** @brief Reports whether prepared RR full-screen debugging is selected. */
		bool IsRRDebugVisible() const { return rrFullscreenSource != 0; }
		/** @brief Releases full-screen debug ownership without altering reconstruction. */
		void ClearRRDebugOverlay() { rrFullscreenSource = 0; }
		/** @brief Selects temporary prepared RR views independently of reconstruction controls. */
		void SetRRDebug(uint32_t a_source, uint32_t a_signal, uint32_t a_buffer, float a_exposure);
		ID3D11ShaderResourceView* RRDebugPreview() const { return rrDebugPreviewValid && rrDebugPreview ? rrDebugPreview->srv.get() : nullptr; }

	private:
		static constexpr uint32_t kMaximumInstances = 8192;
		static constexpr uint32_t kGridDivisor = 4;
		static constexpr float kRayMinimum = 0.5f;
		static constexpr float kRayMaximum = 60000.0f;
		static constexpr float kSunAngularRadius = 0.00465f;
		static constexpr uint32_t kLightSnapshotTexels = 8;
		static constexpr uint32_t kLightSnapshotWidth = kLightSnapshotTexels * kMaximumDirectLights;
		static constexpr uint32_t kDirectSignalCount = 7;
		static constexpr uint32_t kShadowSignalCount = 3;
		static constexpr uint32_t kFrameDescriptorCount = 3 + kDirectSignalCount + kShadowSignalCount;
		static constexpr uint32_t kGeometryDescriptorBase = 2 * kFrameDescriptorCount;
		static constexpr uint32_t kAttributeDescriptorBase = kGeometryDescriptorBase + kMaximumInstances;
		static constexpr uint32_t kTextureDescriptorBase = kAttributeDescriptorBase + kMaximumInstances;
		static constexpr uint64_t kBindingOverheadBytes = 1024 * 1024;
		struct Bindings
		{
			std::shared_ptr<const TopLevel> scene;
			winrt::com_ptr<ID3D12Resource> offsets;
			winrt::com_ptr<ID3D12DescriptorHeap> heap;
			std::shared_ptr<const MaterialResources> materials;
			uint64_t bytes = 0;
		};
		struct Frame
		{
			std::shared_ptr<const TopLevel> scene;
			std::shared_ptr<const Bindings> bindings;
			std::unique_ptr<WrappedResource> receivers, output, light;
			// DX12-only UAVs: no DX11 sharing, CPU readback, or intermediate handoff.
			// R32G32_UINT: visible/valid per-light bitsets; R16G16_FLOAT: visibility/confidence.
			winrt::com_ptr<ID3D12Resource> visibilityBits, shadowMask;
            winrt::com_ptr<ID3D12Resource> shadowTemporal;
            std::unique_ptr<WrappedResource> shadowMotion;
            winrt::com_ptr<ID3D12DescriptorHeap> shadowTemporalHeap;
            // Keep D3D12 state objects alive until this frame's commands retire,
            // including shader reloads while an old frame is still executing.
            winrt::com_ptr<ID3D12RootSignature> shadowTemporalRootRef;
            winrt::com_ptr<ID3D12PipelineState> shadowTemporalPipelineRef;
            bool shadowTemporalEnabled = false;
            bool shadowTemporalValid = false;
            bool shadowMotionCaptured = false;
            int32_t shadowHistorySourceSlot = -1; // protects prior resource lifetime until consumer retires
			// DX11-visible transfer only for final Shadow Only composition.
			std::unique_ptr<WrappedResource> shadowMaskExport;
            std::array<std::unique_ptr<WrappedResource>, kHybridInputCount> hybridInputs;
            bool hybridInputsReady = false;
            winrt::com_ptr<ID3D12Resource> hybridShadowedSource;
            winrt::com_ptr<ID3D12Resource> hybridLinearDepth;
            bool hybridShadowedSourceReady = false;
            NativeHybridGI::FrameOutputs nativeGI;
            NativeHybridGI::FrameOutputs postHybridGI; // enhanced tiers: second raster HybridGI over precomposed RT
            bool nativeGIRequested = false;
            uint32_t nativeGIFrame = 0;
			std::array<std::unique_ptr<WrappedResource>, kDirectSignalCount> directSignals;
			winrt::com_ptr<ID3D12RootSignature> root;
			winrt::com_ptr<ID3D12PipelineState> pipeline;
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> commands;
			winrt::com_ptr<ID3D11Buffer> grid, camera;
			std::array<uint32_t, 2> size{};
			uint32_t number = 0, descriptorBase = 0;
			uint32_t materialView = 0;
			uint32_t lightReference = 0, lightBase = 0;
			std::vector<std::array<uint32_t, 2>> lightIdentities;
		std::vector<PreparedPointLight> preparedLights;
			bool allPreparedLights = false;
			float4 strengths{ 1, 1, 1, 1 };
			float4 falloffParameters{ 1.0f, 0.1f, 1.0f, 0.0f };
			float4 stochasticParameters{ 0.0f, 1.0f, 0.0f, 0.0f };
			bool useIndirect = true, useDirectRR = false;
			float indirectStrength = 1.0f;
			float rasterBlend = 1.0f;
			uint32_t normalMode = 2;
			uint32_t directTraceMode = 0, indirectTraceMode = 0, directRRResolutionMode = 0, rrMaterialPolicy = 0;
			uint32_t indirectRays = 0;
            uint32_t transportTier = 0; // GPU-only compositor transport diagnostic
			std::array<uint32_t, 128> roomReferences{};
			uint64_t fence = 0, retirement = 0, generation = 0, bytes = 0;
			bool submitted = false;
		};
		struct PendingBindings
		{
			std::atomic<bool> busy{ true };
			std::shared_ptr<const Bindings> value;
			std::shared_ptr<const TopLevel> scene;
			std::string failure;
			uint64_t generation = 0, bytes = 0;
		};
		void CreatePipeline();
		static std::shared_ptr<const Bindings> CreateBindings(const GraphicsBackend& a_backend, std::shared_ptr<const TopLevel> a_scene);
		static uint64_t BindingBytes(const GraphicsBackend& a_backend, const TopLevel& a_scene);
		void PrepareReceivers(Frame& a_frame, const std::array<uint32_t, 2>& a_nativeSize, const LightLimitFixData::LightView& a_lights, ID3D11ShaderResourceView* a_roomMetadata);
        void PrepareHybridInputs(Frame& a_frame);
        void RecordHybridShadowedSource(Frame& a_frame);
        void RecordShadowTemporal(Frame& a_frame);
        ID3D12Resource* EffectiveShadowMask(const Frame& frame) const
        {
            return frame.shadowTemporalValid && frame.shadowTemporal ?
                frame.shadowTemporal.get() : frame.shadowMask.get();
        }
        void RecordNativeHybridGI(Frame& a_frame);
        void RecordAdvancedTransport(Frame& a_frame);
		void Record(Frame& a_frame, const float3& a_direction, bool a_localLight);
		void Consume(Frame& a_frame);
		void CompositeScene(Frame& a_frame, ID3D11UnorderedAccessView* a_target);
		/** @brief Reconstructs the current separated direct signals using matching RT material guides. */
		uint32_t DenoiseDirect(Frame& a_frame);
		void PrepareRRPreview(Frame& a_frame, uint32_t a_rrFlags);
		uint64_t ResidentBytes() const;
		uint64_t MaterialResidentBytes() const;
		GraphicsBackend backend;
		SharedFence producer, completion;
		winrt::com_ptr<ID3D11ComputeShader> prepare, display, composite;
        winrt::com_ptr<ID3D11ComputeShader> prepareHybrid;
        winrt::com_ptr<ID3D12RootSignature> shadowTemporalRoot;
        winrt::com_ptr<ID3D12PipelineState> shadowTemporalPipeline;
        uint64_t shadowTemporalDispatches = 0, shadowTemporalReprojections = 0;
        std::string shadowTemporalFailure;
        winrt::com_ptr<ID3D11ComputeShader> nativeHybridResolve;
        winrt::com_ptr<ID3D12RootSignature> hybridSourceRoot;
        winrt::com_ptr<ID3D12PipelineState> hybridSourcePipeline;
        winrt::com_ptr<ID3D12DescriptorHeap> hybridSourceHeap;
        std::unique_ptr<NativeHybridGI> nativeHybridGI;
        std::unique_ptr<NativeHybridGI> postHybridGI;
        RedstoneDenoiser advancedDenoiser;
        uint64_t advancedComposeSubmissions = 0, postHybridComposeSubmissions = 0;
        std::string advancedComposeFailure;
        uint64_t nativeHybridPreviewSubmissions = 0;
        uint64_t nativeHybridFallbackFrames = 0;
        std::string nativeHybridPreviewFailure;
		winrt::com_ptr<ID3D11ComputeShader> prepareDirectGuides;
		winrt::com_ptr<ID3D11ComputeShader> packRRSignals;
		std::unique_ptr<ConstantBuffer> rrGridSettings;
		std::unique_ptr<ConstantBuffer> lightingCompositeSettings;
		RedstoneDenoiser directDenoiser;
		RedstoneDenoiser indirectDenoiser;
		std::array<std::unique_ptr<Texture2D>, 13> directRRTextures;
		uint64_t directRRPolicyRevision = UINT64_MAX;
		uint32_t directRRIndirectRays = 0;
		uint32_t directRRNormalMode = UINT32_MAX;
		uint32_t directRRMaterialPolicy = UINT32_MAX;
		bool directRRCheckerboard = false;
		uint32_t directRRPhase = 0;
		uint32_t rrPackFrame = UINT32_MAX;
		std::unique_ptr<Texture2D> rrDebugPreview;
		std::unique_ptr<ConstantBuffer> rrDebugSettings;
		winrt::com_ptr<ID3D11ComputeShader> rrDebugShader;
		uint32_t rrFullscreenSource = 0, rrSignalView = 0, rrBuffer = 2, rrViewerRequestFrame = UINT32_MAX;
		float rrPreviewExposure = 0.0f;
		bool rrDebugPreviewValid = false;
		bool rrDebugFailed = false;
		uint32_t rrDebugPreviewFrame = 0;
		std::array<uint32_t, 2> rrDebugStorageSize{};
		bool rrDebugIsInput = false, rrDebugUnavailable = false;
		std::array<uint32_t, 2> directRRSize{};
		uint32_t directRRFrame = 0;
		uint64_t directRRGeneration = UINT64_MAX;
		float4 directRRStrengths{};
		float4 directRRFalloff{};
		float4 directRRStochastic{};
		std::vector<std::array<uint32_t, 2>> directRRIdentities;
		uint64_t directRRDispatches = 0, indirectCompositeSubmissions = 0;
		uint64_t directRRBudgetDeferrals = 0;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		std::array<Frame, 2> frames;
		std::shared_ptr<const Bindings> cachedBindings;
		std::shared_ptr<PendingBindings> pendingBindings;
		uint64_t bindingCreations = 0, bindingDeferrals = 0;
		std::unique_ptr<Texture2D> preview;
		std::array<uint32_t, 2> previewSize{};
		uint64_t previewBytes = 0;
		uint64_t generation = 0, nextCompletion = 0, submissions = 0, completions = 0, previewDrawSubmissions = 0, busyFrames = 0, budgetDeferrals = 0;
		uint32_t previewFrame = 0;
		uint32_t previewMaterialView = 0;
		bool previewValid = false;
		bool localLightMode = false;
		uint32_t sourceLightCount = 0, sourceLightFrame = 0;
		uint32_t sourceReference = 0, sourceBase = 0, sourceRoomGeneration = 0;
		bool sourceRoomMetadata = false;
		bool allPreparedLights = false;
		uint32_t requestedLightCount = 1;
		std::string lightSelectionFailure;
		uint64_t unavailableLightFrames = 0;
		uint64_t directSubmissions = 0, sceneCompositeSubmissions = 0;
		uint64_t directTraceDispatches = 0, indirectTraceDispatches = 0;
        uint64_t advancedTransportDispatches = 0;
		uint64_t shadowMaskTraceDispatches = 0, shadowMaskResolveDispatches = 0;
        uint64_t hybridInputDispatches = 0, hybridInputBudgetDeferrals = 0, hybridInputLastBytes = 0;
        uint32_t hybridInputLastFrame = 0;
        std::string hybridInputFailure;
        uint64_t hybridSourceDispatches = 0;
        uint32_t hybridSourceLastFrame = 0;
        std::string hybridSourceFailure;
		uint32_t sceneCompositeFrame = 0;
		std::string failure;
	};
}

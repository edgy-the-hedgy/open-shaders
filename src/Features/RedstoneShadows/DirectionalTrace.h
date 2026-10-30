#pragma once

#include "AccelerationScene.h"
#include "Buffer.h"
#include "Features/LightLimitFix/LightView.h"
#include "MaterialResources.h"
#include "Features/RedstoneGI/RuntimeDenoiser.h"
#include <atomic>
#include <span>

namespace RedstoneShadow
{
	/** @brief Traces directional blocker distances and publishes an explicitly partial raw preview. */
	class DirectionalTrace
	{
	public:
		static constexpr uint32_t kFirstDirectView = 10;
		static constexpr uint32_t kCombinedDirectView = 12;
		static constexpr uint32_t kMaximumDirectLights = 32;
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
		void Dispatch(std::shared_ptr<const TopLevel> a_scene, const std::array<uint32_t, 2>& a_nativeSize, const float3& a_lightDirection, uint32_t a_frame, uint64_t a_budgetBytes, bool a_localLight = false, const LightLimitFixData::LightView& a_lights = {}, uint32_t a_materialView = 0, ID3D11UnorderedAccessView* a_sceneTarget = nullptr, uint32_t a_lightReference = 0, uint32_t a_lightBase = 0, ID3D11ShaderResourceView* a_roomMetadata = nullptr, std::span<const std::array<uint32_t, 2>> a_lightIdentities = {}, bool a_allPreparedLights = false, float4 a_strengths = { 1, 1, 1, 1 }, bool a_useIndirect = true, float a_indirectStrength = 1.0f, bool a_useDirectRR = false, uint32_t a_indirectRays = 0, std::span<const uint32_t> a_roomReferences = {});
		/** @brief Returns a raw preview view whose image remains owned by this service. */
		ID3D11ShaderResourceView* Preview() const { return previewValid && preview ? preview->srv.get() : nullptr; }
		/** @brief Returns the completed preview dimensions for an undistorted diagnostic image. */
		std::array<uint32_t, 2> PreviewSize() const { return previewSize; }
		/** @brief Records an ImGui preview draw reference separately from scene-lighting consumption. */
		void MarkPreviewDrawSubmitted() { if (Preview()) ++previewDrawSubmissions; }
		/** @brief Reports raw traversal and preview evidence without claiming RR or replacement coverage. */
		json GetDiagnostics() const;
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

	private:
		static constexpr uint32_t kMaximumInstances = 8192;
		static constexpr uint32_t kGridDivisor = 4;
		static constexpr float kRayMinimum = 0.5f;
		static constexpr float kRayMaximum = 60000.0f;
		static constexpr float kSunAngularRadius = 0.00465f;
		static constexpr uint32_t kLightSnapshotTexels = 8;
		static constexpr uint32_t kLightSnapshotWidth = kLightSnapshotTexels * kMaximumDirectLights;
		static constexpr uint32_t kDirectSignalCount = 7;
		static constexpr uint32_t kFrameDescriptorCount = 3 + kDirectSignalCount;
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
			bool allPreparedLights = false;
			float4 strengths{ 1, 1, 1, 1 };
			bool useIndirect = true, useDirectRR = false;
			float indirectStrength = 1.0f;
			uint32_t indirectRays = 0;
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
		winrt::com_ptr<ID3D11ComputeShader> prepareDirectGuides;
		winrt::com_ptr<ID3D11ComputeShader> packRRSignals;
		std::unique_ptr<ConstantBuffer> rrGridSettings;
		std::unique_ptr<ConstantBuffer> lightingCompositeSettings;
		RedstoneDenoiser directDenoiser;
		RedstoneDenoiser indirectDenoiser;
		std::array<std::unique_ptr<Texture2D>, 13> directRRTextures;
		uint64_t directRRPolicyRevision = UINT64_MAX;
		uint32_t directRRIndirectRays = 0;
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
		uint32_t sceneCompositeFrame = 0;
		std::string failure;
	};
}

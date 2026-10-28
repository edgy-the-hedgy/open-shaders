#pragma once

#include "AccelerationScene.h"
#include "Buffer.h"

namespace RedstoneShadow
{
	/** @brief Traces directional blocker distances and publishes an explicitly partial raw preview. */
	class DirectionalTrace
	{
	public:
		/** @brief Loads packaged DXIL using the existing device; compiles only native receiver/preview passes. */
		bool Initialize(const GraphicsBackend& a_backend);
		/** @brief Preserves in-flight ray resources during early teardown. */
		~DirectionalTrace();
		/** @brief Invalidates publication while pending frames retire by fence. */
		void Reset();
		/** @brief Retries shader loading after an explicit cache reload. */
		void ReloadShaders();
		/** @brief Retires frames and consumes completed raw output without CPU waits. */
		void Retire();
		/** @brief Traces a bounded current native receiver grid against a retained completed TLAS. */
		void Dispatch(std::shared_ptr<const TopLevel> a_scene, const std::array<uint32_t, 2>& a_nativeSize, const float3& a_lightDirection, uint32_t a_frame, uint64_t a_budgetBytes);
		/** @brief Returns a raw preview view whose image remains owned by this service. */
		ID3D11ShaderResourceView* Preview() const { return previewValid && preview ? preview->srv.get() : nullptr; }
		/** @brief Records an ImGui preview draw reference separately from scene-lighting consumption. */
		void MarkPreviewDrawSubmitted() { if (Preview()) ++previewDrawSubmissions; }
		/** @brief Reports raw traversal and preview evidence without claiming RR or replacement coverage. */
		json GetDiagnostics() const;

	private:
		static constexpr uint32_t kMaximumInstances = 8192;
		static constexpr uint32_t kGridDivisor = 4;
		static constexpr float kRayMinimum = 0.5f;
		static constexpr float kRayMaximum = 60000.0f;
		static constexpr float kSunAngularRadius = 0.00465f;
		struct Frame
		{
			std::shared_ptr<const TopLevel> scene;
			std::unique_ptr<WrappedResource> receivers, output;
			winrt::com_ptr<ID3D12Resource> offsets;
			winrt::com_ptr<ID3D12DescriptorHeap> heap;
			winrt::com_ptr<ID3D12RootSignature> root;
			winrt::com_ptr<ID3D12PipelineState> pipeline;
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> commands;
			winrt::com_ptr<ID3D11Buffer> grid, camera;
			std::array<uint32_t, 2> size{};
			uint32_t number = 0;
			uint64_t fence = 0, retirement = 0, generation = 0, bytes = 0;
			bool submitted = false;
		};
		void CreatePipeline();
		void PrepareReceivers(Frame& a_frame, const std::array<uint32_t, 2>& a_nativeSize);
		void Record(Frame& a_frame, const float3& a_direction);
		void Consume(Frame& a_frame);
		uint64_t ResidentBytes() const;
		GraphicsBackend backend;
		SharedFence producer, completion;
		winrt::com_ptr<ID3D11ComputeShader> prepare, display;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		std::array<Frame, 2> frames;
		std::unique_ptr<Texture2D> preview;
		std::array<uint32_t, 2> previewSize{};
		uint64_t previewBytes = 0;
		uint64_t generation = 0, nextCompletion = 0, submissions = 0, completions = 0, previewDrawSubmissions = 0, busyFrames = 0, budgetDeferrals = 0;
		uint32_t previewFrame = 0;
		bool previewValid = false;
		std::string failure;
	};
}

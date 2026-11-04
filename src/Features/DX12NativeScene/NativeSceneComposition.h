#pragma once
#include "Buffer.h"
#include "GraphicsBackend.h"
#include "GraphicsInterop.h"
#include "NativeSceneOutput.h"

namespace DX12Native
{
	/** @brief Borrows independent shadow visibility and its image-layer controls. */
	struct NativeShadowLayer
	{
		NativeSceneOutput gpu;
		std::array<float, 3> origin{};
		float strength = 0, blend = 1, radius = 0, tolerance = 2;
		bool preview = false;
	};
	/** @brief Owns the final scene handoff and GPU composition, without selecting producer policy. */
	class NativeSceneComposition
	{
	public:
		/** @brief Retains submitted resources through both GPU queues during shutdown. */
		~NativeSceneComposition();
		/** @brief Composes current contributions once in DX12 and copies the finished scene back to DX11. */
		std::optional<NativeSceneOutput> Present(const GraphicsBackend& graphics, ID3D11UnorderedAccessView* target,
			const std::array<uint32_t, 2>& size, const std::optional<NativeSceneOutput>& direct,
			const std::optional<NativeSceneOutput>& hybrid, uint64_t budget,
			const std::optional<NativeShadowLayer>& shadow = std::nullopt);
		/** @brief Invalidates cached shaders while frames retain submitted pipeline ownership. */
		void Reload();
		/** @brief Captures already shadowed raster radiance and orders its handoff before native producers. */
		void CaptureSource(const GraphicsBackend& graphics, ID3D11ShaderResourceView* diffuse, ID3D11ShaderResourceView* specular,
			const std::array<uint32_t, 2>& size, uint32_t frameNumber, uint64_t budget);
		/** @brief Returns only the current frame's immutable, GPU-resident shared scene input. */
		std::optional<NativeSceneOutput> GetSceneInput(uint32_t frameNumber, const std::array<uint32_t, 2>& size) const;
		/** @brief Reports source, final scene submissions and handoffs separately from visual validation. */
		json GetDiagnostics() const;
		/** @brief Returns the current image-layer admission failure without constructing diagnostics. */
		const std::string& ShadowFailure() const { return shadowFailure; }
		/** @brief Returns the current final-composition failure without constructing diagnostics. */
		const std::string& Failure() const { return failure; }

	private:
		struct Frame
		{
			std::unique_ptr<WrappedResource> input, output, legacy, shadowDepth;
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> commands;
			winrt::com_ptr<ID3D12Resource> camera;
			std::unique_ptr<ConstantBuffer> legacySettings, legacyCamera;
			std::shared_ptr<const void> bindings;
			GraphicsBackend backend;
			uint64_t fence = 0, retirement = 0;
		};
		bool CaptureLegacy(Frame& frame, std::array<uint32_t, 2> size, float strength);
		bool CaptureShadowDepth(Frame& frame, std::array<uint32_t, 2> dimensions, uint64_t budget);
        uint64_t ResidentBytes() const;
        void TrimIdleFrames(Frame* keep = nullptr);
		std::array<Frame, 2> frames;
		GraphicsBackend backend;
		SharedFence producer, completion;
		NativeCompute composition;
		winrt::com_ptr<ID3D11ComputeShader> legacyShader;
		winrt::com_ptr<ID3D11ComputeShader> captureSource;
		winrt::com_ptr<ID3D11ComputeShader> captureShadowDepth;
		std::array<std::shared_ptr<WrappedResource>, 2> initialSources;
		std::array<uint64_t, 2> sourceReady{};
		std::unique_ptr<ConstantBuffer> sourceSettings;
		uint32_t sourceSlot = 0, sourceFrame = UINT32_MAX;
		bool sourceValid = false;
		std::array<uint32_t, 2> sourceSize{};
		std::string sourceSelectedProvider;
		uint64_t sourceHandoffs = 0;
		uint64_t submissions = 0, handoffs = 0, busy = 0;
		std::string failure;
		std::string shadowFailure;
		uint64_t shadowSubmissions = 0;
	};
}

#pragma once

#include "Features/DX12NativeScene/VideoMemoryBudget.h"
#include "Features/NativeLighting/NativeLighting.h"
#include "GraphicsInterop.h"
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

/** @brief Copies the completed raster scene into a DX12-owned frame. */
class NativeComposite
{
public:
	/** @brief Interpretation of the source values; the pass performs no conversion. */
	enum class Transfer
	{
		Unknown,
		Linear,
		LegacyScene
	};
	enum class Gamut
	{
		Unknown
	};
	struct ShadowSettings
	{
		float contribution = 1.0f, blend = 1.0f, radiusPixels = 0.0f, receiverTolerance = 2.0f;
		bool preview = false, afterLighting = false, spatialReconstruction = true;
		uint64_t revision = 1;
	};
	struct Policy
	{
		uint32_t diffuse = 1;  // Effective validity: display reference-compatible recovered contributions.
		uint32_t specular = 1;
		uint32_t visual = 0;
	};
	struct SourceBlendSettings
	{
		bool enabled = false;
		float mix = 0.5f, nativeIntensity = 1.0f, rasterIntensity = 1.0f;
	};
	enum class Guide : size_t { Depth, Motion, Reactive, Transparency, Count };
	static constexpr size_t GuideCount = static_cast<size_t>(Guide::Count);
	using RasterGuideInputs = std::array<ID3D11Texture2D*, GuideCount>;
	struct GuideCompareSettings
	{
		std::array<float, GuideCount> tolerance{ 2.0f, 0.5f, 0.05f, 0.05f };
		std::array<uint32_t, GuideCount> priority{};  // 0=native unless within, 1=raster unless within
	};
	struct GuideSurface
	{
		winrt::com_ptr<ID3D12Resource> resource;
		winrt::com_ptr<ID3D12Resource> coverage;  // R32_UINT: 1=authoritative native sample, 0=unavailable
		winrt::com_ptr<ID3D12Resource> origin;    // Selected source per pixel: 0=raster, 1=native
		uint32_t width = 0, height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		bool rasterOrigin = false;
		bool nativeOrigin = false;
	};
	struct Frame
	{
		winrt::com_ptr<ID3D12Resource> color;
		winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
		winrt::com_ptr<ID3D12Fence> producerFence;
		std::array<GuideSurface, GuideCount> rasterGuides;
		std::array<GuideSurface, GuideCount> nativeGuides;
		std::array<GuideSurface, GuideCount> selectedGuides;
		uint64_t producerValue = 0;
		uint64_t validityRevision = 0;
		uint32_t frame = 0, width = 0, height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		Transfer transfer = Transfer::Unknown;
		Gamut gamut = Gamut::Unknown;
		D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
		bool valid = false;
		bool lightingSubmitted = false;
		bool shadowSubmitted = false;
		/** @brief Reports whether the producer fence completed without device removal. */
		bool IsComplete() const
		{
			if (!valid || !producerFence || !producerValue)
				return false;
			const auto completed = producerFence->GetCompletedValue();
			return completed != UINT64_MAX && completed >= producerValue;
		}
	};

	/** @brief Captures one frame without waiting for the GPU on the CPU. */
	std::shared_ptr<const Frame> Capture(ID3D11Device5* a_device11, ID3D11DeviceContext4* a_context11,
		ID3D12Device* a_device12, ID3D12CommandQueue* a_queue, ID3D11Texture2D* a_source,
		uint32_t a_frame, uint32_t a_width, uint32_t a_height, Transfer a_transfer,
		const std::shared_ptr<const NativeLightingFeature::Contribution>& a_lighting,
		const std::shared_ptr<const DX12Native::ShadowFrame>& a_shadow,
		const std::shared_ptr<const DX12Native::RasterFrame>& a_raster,
		const ShadowSettings& a_shadowSettings,
		float a_strength, bool a_directSpatialReconstruction, const Policy& a_policy, uint64_t a_revision,
		uint32_t a_nativeReadinessStage = 0, bool a_includeRasterScene = true, bool a_originPreview = false, bool a_ignoreValidity = false,
		const SourceBlendSettings& a_sourceBlend = {}, const RasterGuideInputs& a_rasterGuides = {},
		const GuideCompareSettings& a_guideCompare = {});
	/** @brief Retains a frame until a downstream consumer's GPU fence completes. */
	bool Retain(const std::shared_ptr<const Frame>& a_frame, ID3D12Fence* a_fence, uint64_t a_value);
	/** @brief Returns a short explanation of the latest capture result. */
	const std::string& Status() const { return status; }
	/** @brief The first rejected independent shadow gate or successful submission. */
	const std::string& ShadowStatus() const { return shadowStatus; }
	uint64_t CompletedComposites() const { return completedComposites.load(); }
	uint64_t AcceptedPixels() const { return acceptedPixels.load(); }
	uint64_t ModifiedPixels() const { return modifiedPixels.load(); }
	uint64_t VisualizationPixels() const { return visualizationPixels.load(); }
	uint64_t EffectiveRecoveredPixels() const { return effectiveRecoveredPixels.load(); }
	uint64_t FiniteOnlyPixels() const { return finiteOnlyPixels.load(); }
	uint64_t CounterReadFailures() const { return counterReadFailures.load(); }
	uint64_t ReconstructedPixels() const { return reconstructedPixels.load(); }
	~NativeComposite();

private:
	struct Slot
	{
		std::shared_ptr<Frame> frame;
		std::unique_ptr<WrappedResource> shared;
		std::array<std::unique_ptr<WrappedResource>, GuideCount> guideCopies;
		std::array<D3D11_TEXTURE2D_DESC, GuideCount> guideDescs{};
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList> commands;
		winrt::com_ptr<ID3D12DescriptorHeap> lightingDescriptors, shadowDescriptors, originDescriptors;
		winrt::com_ptr<ID3D12DescriptorHeap> blendDescriptors;
		winrt::com_ptr<ID3D12DescriptorHeap> nativeDepthDescriptors;
		winrt::com_ptr<ID3D12DescriptorHeap> nativeMotionDescriptors;
		winrt::com_ptr<ID3D12DescriptorHeap> nativeMaskDescriptors;
		std::array<winrt::com_ptr<ID3D12DescriptorHeap>, GuideCount> guideCompareDescriptors;
		winrt::com_ptr<ID3D12DescriptorHeap> clearDescriptors;
		winrt::com_ptr<ID3D12RootSignature> shadowRoot;
		winrt::com_ptr<ID3D12PipelineState> shadowPipeline;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12RootSignature> originRoot;
		winrt::com_ptr<ID3D12PipelineState> originPipeline;
		winrt::com_ptr<ID3D12RootSignature> blendRoot;
		winrt::com_ptr<ID3D12PipelineState> blendPipeline;
		winrt::com_ptr<ID3D12RootSignature> depthRoot, motionRoot;
		winrt::com_ptr<ID3D12PipelineState> depthPipeline, motionPipeline;
		winrt::com_ptr<ID3D12RootSignature> maskRoot;
		winrt::com_ptr<ID3D12PipelineState> maskPipeline;
		winrt::com_ptr<ID3D12RootSignature> guideCompareRoot;
		winrt::com_ptr<ID3D12PipelineState> guideComparePipeline;
		winrt::com_ptr<ID3D12Resource> originMask;
		winrt::com_ptr<ID3D12Resource> nativeColor;
		winrt::com_ptr<ID3D12Resource> nativeDepth, nativeDepthCoverage;
		winrt::com_ptr<ID3D12Resource> nativeMotion, nativeMotionCoverage;
		winrt::com_ptr<ID3D12Resource> nativeReactive, nativeTransparency, nativeMaskCoverage;
		std::array<winrt::com_ptr<ID3D12Resource>, GuideCount> comparedGuides, comparedOrigins;
		std::shared_ptr<const DX12Native::TopLevel> nativeDepthScene;
		winrt::com_ptr<ID3D12Resource> counters, counterReset, counterReadback;
		SharedFence fence;
		std::vector<std::pair<winrt::com_ptr<ID3D12Fence>, uint64_t>> consumers;
		ID3D12Device* device = nullptr;
		ID3D11Device* device11 = nullptr;
		D3D11_TEXTURE2D_DESC sourceDesc{};
		bool supportsLighting = false;
		bool originPreview = false;
		bool sourceBlendEnabled = false;
		bool nativeDepthEnabled = false;
		bool nativeMotionEnabled = false;
		bool nativeMasksEnabled = false;
		std::array<bool, GuideCount> comparisonAllocated{};
		uint64_t frameBytes = 0;
		bool countersRead = true;
		bool untracked = false;
	};
	std::array<std::unique_ptr<Slot>, 4> slots;
	winrt::com_ptr<ID3D12RootSignature> root, originRoot;
	winrt::com_ptr<ID3D12PipelineState> pipeline, originPipeline;
	winrt::com_ptr<ID3D12RootSignature> blendRoot;
	winrt::com_ptr<ID3D12PipelineState> blendPipeline;
	winrt::com_ptr<ID3D12RootSignature> nativeDepthRoot;
	winrt::com_ptr<ID3D12PipelineState> nativeDepthPipeline;
	winrt::com_ptr<ID3D12RootSignature> nativeMotionRoot;
	winrt::com_ptr<ID3D12PipelineState> nativeMotionPipeline;
	winrt::com_ptr<ID3D12RootSignature> nativeMaskRoot;
	winrt::com_ptr<ID3D12PipelineState> nativeMaskPipeline;
	winrt::com_ptr<ID3D12RootSignature> guideCompareRoot;
	std::array<winrt::com_ptr<ID3D12PipelineState>, 3> guideComparePipelines;
	ID3D12Device *pipelineDevice = nullptr, *shadowPipelineDevice = nullptr, *originPipelineDevice = nullptr, *blendPipelineDevice = nullptr, *nativeDepthPipelineDevice = nullptr, *nativeMotionPipelineDevice = nullptr, *nativeMaskPipelineDevice = nullptr, *guideComparePipelineDevice = nullptr;
	winrt::com_ptr<ID3D12RootSignature> shadowRoot;
	winrt::com_ptr<ID3D12PipelineState> shadowPipeline;
	std::atomic<uint64_t> completedComposites{ 0 }, acceptedPixels{ 0 }, modifiedPixels{ 0 }, visualizationPixels{ 0 }, counterReadFailures{ 0 };
	std::atomic<uint64_t> effectiveRecoveredPixels{ 0 }, finiteOnlyPixels{ 0 }, reconstructedPixels{ 0 };
	DX12Native::VideoMemoryBudget videoBudget;
	bool InitializeLighting(ID3D12Device* a_device);
	bool InitializeShadow(ID3D12Device* a_device);
	bool InitializeOriginPreview(ID3D12Device* a_device);
	bool InitializeSourceBlend(ID3D12Device* a_device);
	bool InitializeNativeDepth(ID3D12Device* a_device);
	bool InitializeNativeMotion(ID3D12Device* a_device);
	bool InitializeNativeMasks(ID3D12Device* a_device);
	bool InitializeGuideComparator(ID3D12Device* a_device);
	std::optional<globals::FrameBuffer> previousNativeCamera;
	std::shared_ptr<const DX12Native::TopLevel> previousMotionScene;
	uint32_t previousNativeFrame = 0;
	std::string status = "Not captured";
	std::string shadowStatus = "Independent shadow layer not captured";
	std::string shadowPipelineFailure;
};

#pragma once

#include "DX12NativeAcceleration.h"
#include "DX12NativeRasterCapture.h"
#include "DX12ProjectedInputs.h"
#include "DX12GpuRetirement.h"
#include "DX12MaterialFrameContract.h"
#include "Features/NativePBR/NativeRawInputCapture.h"
#include "../../../package/Shaders/Common/NativeShadowRT.hlsli"
#include "Utils/FileSystem.h"
#include "Utils/RuntimeTaskGate.h"
#include <atomic>
#include <unordered_map>
#include <unordered_set>

namespace DX12Native
{
	// Optional visible-pixel material generation. No final-color writes or CPU fence waits.
	// Full-resolution results stay on GPU; only aggregate counts are read back.
	class NativeScreenProbe
	{
		using Eval = NativeMaterialEvaluation;
		static constexpr uint32_t kWidth = 64, kHeight = 36;
		static constexpr uint32_t kCounterCount = 12;
		static constexpr uint64_t kCounterBytes = kCounterCount * sizeof(uint32_t);
		static constexpr uint32_t kSampleStride = MaterialFrameContract::kMaterialStride; // 16 IEEE binary16 values, same component ordering as Eval::Sample.
		struct Camera
		{
			Matrix projectionInverse, viewInverse;
			float4 positionAdjust;
			uint32_t width, height, gridWidth, gridHeight, collectStats, parallaxEnabled, parallaxWarpFixEnabled, parallaxShadowEnabled;
			float mipBias;
			uint32_t projectionRows, projectionFrame, hasRawInput, quality, transformCount, rasterBindingCount, rasterRecordCount, sceneRecordCount, rasterSourceCount, hasSecondaryScene, hasRasterRayMapping, rasterPreparedSourceCount;
		};
		static_assert(sizeof(Camera) <= 256);
		struct OutputAllocation
		{
			winrt::com_ptr<ID3D12Resource> resource, rayIdentity, materialLobes;
			uint64_t bytes=0, identityBytes=0, lobeBytes=0, allocationBytes=0;
			bool shaderReadable=false;
		};
		struct Submission
		{
			std::shared_ptr<const RasterFrame> raster;
			std::shared_ptr<const TopLevel> scene;
			std::shared_ptr<const ProjectedInputFrame> projection;
			std::shared_ptr<const NativePBR::RawInputFrame> rawInputs;
			std::shared_ptr<const Eval> rasterEvaluation;
			std::vector<Eval::Upload> rasterUploads;
			winrt::com_ptr<ID3D12Resource> projectionLookup, transforms, emptyTable, rasterMaterialLookup, rasterProjectionLookup;
			std::shared_ptr<OutputAllocation> output;
			winrt::com_ptr<ID3D12Resource> readback, camera, counters, counterInit;
			winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
			winrt::com_ptr<ID3D12RootSignature> root;
			winrt::com_ptr<ID3D12PipelineState> pipeline;
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> commands;
			winrt::com_ptr<ID3D12Fence> fence;
			uint64_t value = UINT64_MAX;
			uint32_t frame = 0, width = 0, height = 0, scope = 0;
			uint64_t geometryRevision = 0;
			bool fullResolution = false, collectStats = false;
			bool discard = false;
		};
		std::array<std::shared_ptr<Submission>, 3> submissions;
		std::array<std::shared_ptr<OutputAllocation>, 4> outputPool;
		VideoMemoryBudget videoBudget;
		GraphicsBackend owner;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		winrt::com_ptr<ID3D12Fence> fence;
		uint64_t nextValue = 0;
		uint32_t lastFrame = UINT32_MAX;
		std::weak_ptr<const TopLevel> transformScene;
		winrt::com_ptr<ID3D12Resource> transformTable;
		uint32_t transformCount = 0;
		using Source = std::shared_ptr<const DX12::PreparedMaterialSource>;
		struct RasterPreparation
		{
			uint32_t scope = 0;
			uint64_t revision = 0;
			std::vector<Source> sources;
			std::shared_ptr<const Eval> seed, previous;
			std::shared_ptr<const NativePBR::MaterialCatalogSnapshot> catalog;
			std::shared_ptr<const Eval> result;
			std::vector<Eval::Upload> uploads;
			std::string failure;
			std::atomic<bool> finished{false};
		};
		std::shared_ptr<RasterPreparation> rasterJob;
		std::shared_ptr<const Eval> rasterCache;
		std::shared_ptr<const NativePBR::MaterialCatalogSnapshot> rasterCatalog;
		std::vector<Source> rasterSources;
		uint64_t nextRasterRevision = 0;
		uint32_t rasterCacheScope = 0, lastRasterFailure = UINT32_MAX;
		static constexpr size_t kMaxRasterPreparationSources = 32768;
		static constexpr uint64_t kMaxRasterSnapshotBytes = 32ull * VideoMemoryBudget::kMiB;

		// Source identities are immutable snapshots, not frame-local ordinals.
		// A value-equal replacement may reuse a cached row even after the
		// engine-side snapshot cache has evicted its original pointer.
		static std::vector<uint32_t> MapRasterSources(std::span<const Source> current, std::span<const Source> prepared)
		{
			std::vector<uint32_t> result(current.size(),UINT32_MAX);
			std::unordered_map<const DX12::PreparedMaterialSource*,uint32_t> exact;
			std::unordered_multimap<std::string_view,uint32_t> byTexture;
			exact.reserve(prepared.size()); byTexture.reserve(prepared.size());
			for (uint32_t i=0; i<prepared.size(); ++i) if (prepared[i]) {
				exact.emplace(prepared[i].get(),i);
				byTexture.emplace(prepared[i]->texturePaths[0],i);
			}
			for (uint32_t i=0; i<current.size(); ++i) {
				if (!current[i]) continue;
				if (const auto it=exact.find(current[i].get()); it!=exact.end()) { result[i]=it->second; continue; }
				const auto range=byTexture.equal_range(current[i]->texturePaths[0]);
				for (auto it=range.first; it!=range.second; ++it)
					if (*current[i]==*prepared[it->second]) { result[i]=it->second; break; }
			}
			return result;
		}

		static bool CompatibleTextureNamespace(const Eval* prepared, const Eval* scene)
		{
			return !scene || (prepared && prepared->poolTextures.size()>=scene->poolTextures.size() &&
				std::equal(scene->poolTextures.begin(),scene->poolTextures.end(),prepared->poolTextures.begin()));
		}

		void QueueRasterPreparation(const GraphicsBackend& backend, const std::shared_ptr<Util::RuntimeTaskGate>& gate,
			uint32_t scope, uint32_t frame, std::span<const Source> sources,
			std::shared_ptr<const Eval> seed, std::shared_ptr<const NativePBR::MaterialCatalogSnapshot> catalog)
		{
			if (!gate || rasterJob || (lastRasterFailure!=UINT32_MAX && frame-lastRasterFailure<30u)) return;
			auto job=std::make_shared<RasterPreparation>();
			job->scope=scope; job->revision=++nextRasterRevision; job->catalog=std::move(catalog);
			job->seed=std::move(seed); job->previous=rasterCache;
			uint64_t bytes=0;
			std::unordered_set<const DX12::PreparedMaterialSource*> unique;
			const auto append=[&](const Source& source) {
				if (!source || unique.contains(source.get()) || job->sources.size()>=kMaxRasterPreparationSources) return;
				const auto size=source->ByteSize();
				if (size>kMaxRasterSnapshotBytes-bytes) return;
				bytes+=size; unique.insert(source.get()); job->sources.push_back(source);
			};
			for (const auto& source:sources) append(source);
			// Preserve already prepared visible material candidates across
			// ordinary frame-order changes, subject to the same bounded budget.
			if (rasterCacheScope==scope && rasterCatalog==job->catalog)
				for (const auto& source:rasterSources) append(source);
			// Even a frame with no acquired material metadata needs an explicit
			// invalid-material publication, not a failed raster submission.
			std::weak_ptr<RasterPreparation> weak=job;
			if (!gate->TrySubmit(Util::RuntimePreparationLane::Material,[weak,device=backend.d3d12Device] {
				const auto retained=weak.lock();
				if (!retained) return;
				try {
					const Eval::TextureCache empty;
					const auto& previous=retained->previous ? retained->previous->textures :
						(retained->seed ? retained->seed->textures : empty);
					retained->result=Eval::PrepareRasterBindings(device.get(),retained->sources,retained->revision,
						retained->catalog.get(),retained->seed.get(),previous,retained->uploads);
					if (!retained->result) retained->failure="Raster residency budget or recipe preparation unavailable";
				} catch (const std::exception& e) { retained->failure=e.what(); }
				catch (...) { retained->failure="Raster material preparation failed"; }
				retained->finished.store(true,std::memory_order_release);
			})) return;
			rasterJob=std::move(job);
		}

		void Initialize(const GraphicsBackend& backend)
		{
			if (pipeline) {
				if (owner.d3d12Device.get() != backend.d3d12Device.get() ||
					owner.commandQueue.get() != backend.commandQueue.get())
					throw std::runtime_error("Screen probe device changed; restart required");
				return;
			}
			auto* device = backend.d3d12Device.get();
			const auto shader = Util::FileHelpers::ReadBoundedBinary(Util::PathHelpers::GetShadersPath() / "NativeComposite" / "NativePBRScreenProbe.dxil", 16 * VideoMemoryBudget::kMiB);
			CD3DX12_DESCRIPTOR_RANGE range;
			range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, NativePBR::kGpuTextureDescriptorCapacity + 1, 3);
			CD3DX12_DESCRIPTOR_RANGE projectionRange;
			projectionRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1 + kProjectedTextureCapacity, 518);
			CD3DX12_DESCRIPTOR_RANGE rawRange;
			rawRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, NativePBR::InputOnly::kPlaneCount + 1, 1031);
			CD3DX12_ROOT_PARAMETER parameters[21];
			parameters[0].InitAsShaderResourceView(0);
			parameters[1].InitAsShaderResourceView(1);
			parameters[2].InitAsShaderResourceView(2);
			parameters[3].InitAsDescriptorTable(1, &range);
			parameters[4].InitAsUnorderedAccessView(2);
			parameters[5].InitAsConstants(1, 0);
			// A descriptor-table SRV can represent a null acceleration structure;
			// a root buffer address must not be repurposed as a fake TLAS.
			CD3DX12_DESCRIPTOR_RANGE sceneRange;
			sceneRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 516);
			parameters[6].InitAsDescriptorTable(1, &sceneRange);
			parameters[7].InitAsConstantBufferView(1);
			parameters[8].InitAsUnorderedAccessView(1);
			parameters[9].InitAsUnorderedAccessView(3);
			parameters[10].InitAsUnorderedAccessView(4);
			parameters[11].InitAsShaderResourceView(517);
			parameters[12].InitAsDescriptorTable(1, &projectionRange);
			parameters[13].InitAsDescriptorTable(1, &rawRange);
			parameters[14].InitAsShaderResourceView(1036); // One record per original draw.
			parameters[15].InitAsShaderResourceView(1037);
			parameters[16].InitAsShaderResourceView(1038);
			parameters[17].InitAsShaderResourceView(1039);
			parameters[18].InitAsShaderResourceView(1040);
			parameters[19].InitAsShaderResourceView(1041);
			parameters[20].InitAsShaderResourceView(1042);
			std::array<CD3DX12_STATIC_SAMPLER_DESC, 4> samplers;
			for (uint32_t mode = 0; mode < 4; ++mode)
				samplers[mode] = CD3DX12_STATIC_SAMPLER_DESC(mode, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
					(mode & 2) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
					(mode & 1) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
					D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
			const CD3DX12_ROOT_SIGNATURE_DESC desc(UINT(std::size(parameters)), parameters, UINT(samplers.size()), samplers.data());
			winrt::com_ptr<ID3DBlob> blob, errors;
			winrt::com_ptr<ID3D12RootSignature> newRoot;
			winrt::com_ptr<ID3D12PipelineState> newPipeline;
			winrt::com_ptr<ID3D12Fence> newFence;
			Eval::Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,blob.put(),errors.put()));
			Eval::Check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(newRoot.put())));
			D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
			pso.pRootSignature=newRoot.get(); pso.CS={shader.data(),shader.size()};
			Eval::Check(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(newPipeline.put())));
			Eval::Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(newFence.put())));
			owner=backend; root=std::move(newRoot); pipeline=std::move(newPipeline); fence=std::move(newFence);
		}
	public:
		struct Stats
		{
			uint32_t frame=0, sampled=0, evaluated=0, noHit=0, depthMismatch=0, missingMaterial=0, invalid=0, baseOnly=0;
			uint32_t width=0, height=0, unsupportedMaterial=0, missingTextures=0, rawInputPixels=0;
			uint32_t primaryRayQueries=0, rasterDerivedPixels=0, unresolvedUntracedPixels=0;
			uint64_t outputBytes=0;
			uint32_t reusedAllocations=0, submittedFrames=0;
		};
		struct MaterialFrame
		{
			// Keep the entire MaterialFrame alive until consumer fence retirement.
			std::shared_ptr<const OutputAllocation> allocation;
			std::shared_ptr<const RasterFrame> raster;
			std::shared_ptr<const TopLevel> scene;
			winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
			winrt::com_ptr<ID3D12Resource> samples, rayIdentity, materialLobes;
			uint32_t rayIdentityRowPitch = 0, materialLobeRowPitch = 0;
			bool parallaxEnabled = false, parallaxShadows = false;
			uint32_t rawInputDraws = 0;
			std::shared_ptr<const NativePBR::RawInputFrame> rawRasterEvidence;
			std::shared_ptr<const Eval> rasterMaterialEvaluation;
			uint64_t rasterMaterialRevision = 0;
			winrt::com_ptr<ID3D12Resource> rasterMaterialLookup;
			uint32_t rasterMaterialSourceCount = 0, rasterUnpreparedSources = 0;
			std::shared_ptr<const ProjectedInputFrame> projection;
			winrt::com_ptr<ID3D12Resource> projectionLookup;
			uint32_t abiVersion=MaterialFrameContract::kMaterialAbiVersion, sampleStride=kSampleStride, rowPitch=0;
			static constexpr D3D12_RESOURCE_STATES publishedState = D3D12_RESOURCE_STATE_COMMON;
			MaterialFrameContract::SurfaceCorrespondence correspondence = MaterialFrameContract::SurfaceCorrespondence::DepthOnly;
			winrt::com_ptr<ID3D12Fence> producerFence;
			uint64_t producerValue=0, geometryRevision=0, materialRevision=0;
			uint32_t frame=0, width=0, height=0, scope=0;
			bool HasSecondaryGeometry() const
			{
				return scene && scope && scene->scope == scope && scene->geometryRevision == geometryRevision &&
					scene->acceleration && scene->producerFence &&
					MaterialFrameContract::ValidProducerSignal(scene->producerValue, scene->producerFence->GetCompletedValue()) &&
					scene->shadingGeometry && scene->shadingGeometry->buffer && scene->shadingGeometry->revision == geometryRevision &&
					scene->materialEvaluation && scene->materialEvaluation->revision == materialRevision &&
					scene->materialEvaluation->instanceCount == scene->shadingGeometry->instanceCount &&
					scene->materialEvaluation->records && scene->materialEvaluation->bindings;
			}
			/** @brief Validates the immutable full-resolution producer contract. */
			bool IsCoherent(uint32_t requestedFrame, uint32_t requestedWidth, uint32_t requestedHeight) const
			{
				if (frame != requestedFrame || width != requestedWidth || height != requestedHeight ||
					!MaterialFrameContract::ValidMaterialABI(abiVersion, sampleStride) ||
					!MaterialFrameContract::ValidLayout(width, height, rowPitch, sampleStride) ||
					!allocation || !samples || allocation->resource.get() != samples.get() ||
					!rayIdentity || allocation->rayIdentity.get() != rayIdentity.get() ||
					!MaterialFrameContract::ValidLayout(width, height, rayIdentityRowPitch, MaterialFrameContract::kRayIdentityStride) ||
					allocation->identityBytes < uint64_t(rayIdentityRowPitch) * height ||
					!materialLobes || allocation->materialLobes.get() != materialLobes.get() ||
					!MaterialFrameContract::ValidLayout(width, height, materialLobeRowPitch, MaterialFrameContract::kMaterialLobeStride) ||
					allocation->lobeBytes < uint64_t(materialLobeRowPitch) * height +
						(parallaxShadows ? uint64_t(rowPitch / kSampleStride) * height * MaterialFrameContract::kParallaxShadowStride : 0u) ||
					!allocation->shaderReadable || !descriptors ||
					!producerFence || !MaterialFrameContract::ValidProducerSignal(producerValue, producerFence->GetCompletedValue()) ||
					!raster || raster->frame != frame || !raster->cameraValid || !raster->depth || !raster->depth->resource ||
					!raster->readyFence || !MaterialFrameContract::ValidProducerSignal(raster->readyValue, raster->readyFence->GetCompletedValue()) ||
					raster->width < width || raster->height < height || !scope ||
					!rawRasterEvidence || !rawRasterEvidence->Matches({frame, scope, 0, 0}, width, height) ||
					!rawRasterEvidence->evidence->winnerCertified || !rasterMaterialEvaluation)
					return false;
				// Secondary geometry can be absent; its validity is not primary
				// material validity. Consumers must call HasSecondaryGeometry().
 				if (rasterMaterialEvaluation) {
					if (!rasterMaterialRevision || rasterMaterialEvaluation->revision != rasterMaterialRevision ||
						!rawRasterEvidence || !rasterMaterialEvaluation->records || !rasterMaterialEvaluation->bindings ||
						!rasterMaterialEvaluation->rasterLandscapeRows ||
						!rasterMaterialLookup || rasterMaterialSourceCount != rawRasterEvidence->materialSources.size() ||
						rasterMaterialLookup->GetDesc().Width < uint64_t(rasterMaterialSourceCount)*sizeof(uint32_t) ||
						rasterMaterialEvaluation->instanceCount < rasterMaterialEvaluation->rasterSourceCount)
						return false;
				} else if (rasterMaterialRevision) return false;
				const auto desc = samples->GetDesc();
				const auto identityDesc = rayIdentity->GetDesc();
				const auto lobeDesc = materialLobes->GetDesc();
				if (lobeDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || lobeDesc.Width < allocation->lobeBytes) return false;
				return identityDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
					identityDesc.Width >= uint64_t(rayIdentityRowPitch) * height && desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
					desc.Width >= uint64_t(rowPitch) * height && allocation->bytes >= uint64_t(rowPitch) * height;
			}
		};
		std::shared_ptr<const MaterialFrame> GetMaterialFrame(uint32_t frame) const
		{
			return published && published->IsCoherent(frame, published->width, published->height) ? published : nullptr;
		}
	private:
		std::shared_ptr<const MaterialFrame> published;
	public:
		Stats stats{};
		uint32_t reusedAllocations=0, submittedFrames=0;
		std::string failure;

		~NativeScreenProbe()
		{
			for (auto& pending : submissions) {
				if (!pending || GpuFenceRetired(pending->fence.get(), pending->value))
					continue;
				std::vector<FenceDependency> dependencies{ FencePoint(pending->fence.get(), pending->value) };
				RetireAfterFences(std::move(pending), owner.d3d12Device, std::move(dependencies));
			}
		}
		void Reset()
		{
			stats={}; failure.clear(); lastFrame=UINT32_MAX; published.reset();
			transformScene.reset(); transformTable = nullptr; transformCount = 0;
			rasterJob.reset(); rasterCache.reset(); rasterCatalog.reset(); rasterSources.clear();
			rasterCacheScope=0; lastRasterFailure=UINT32_MAX;
			reusedAllocations=0; submittedFrames=0;
			for (auto& pending : submissions) if (pending) pending->discard=true;
		}
		void Poll(uint32_t frame)
		{
			if (published && published->frame != frame) published.reset();
			if (fence && fence->GetCompletedValue()==UINT64_MAX) {
				submissions = {}; published.reset(); outputPool = {}; failure="Screen probe device removed"; return;
			}
			for (auto& pending : submissions) PollSubmission(pending);
		}
	private:
		void PollSubmission(std::shared_ptr<Submission>& pending)
		{
			if (!pending) return;
			const auto done=pending->fence->GetCompletedValue();
			if (done==UINT64_MAX) { pending.reset(); published.reset(); return; }
			if (pending->value==UINT64_MAX) {
				if (pending->discard && owner.commandQueue) {
					const auto rescue=nextValue+1;
					if (SUCCEEDED(owner.commandQueue->Signal(pending->fence.get(),rescue))) {
						nextValue=rescue; pending->value=rescue;
					}
				}
				return;
			}
			if (done<pending->value) return;
			if (pending->collectStats && !pending->discard && (!stats.sampled || uint32_t(pending->frame-stats.frame)<0x80000000u)) {
				Stats result{};
				result.frame=pending->frame; result.width=pending->width; result.height=pending->height;
				result.sampled=result.width*result.height;
				result.outputBytes=pending->output->bytes;
				result.reusedAllocations=reusedAllocations; result.submittedFrames=submittedFrames;
				const D3D12_RANGE read{0,SIZE_T(kCounterBytes)};
				void* mapped=nullptr;
				if (SUCCEEDED(pending->readback->Map(0,&read,&mapped))) {
					const auto* counts=static_cast<const uint32_t*>(mapped);
					result.evaluated=counts[0]; result.noHit=counts[1]; result.depthMismatch=counts[2];
					result.missingMaterial=counts[3]; result.invalid=counts[4]; result.baseOnly=counts[5];
					result.unsupportedMaterial=counts[6]; result.missingTextures=counts[7];
					result.rawInputPixels=counts[8];
					result.primaryRayQueries=counts[9]; result.rasterDerivedPixels=counts[10]; result.unresolvedUntracedPixels=counts[11];
					const D3D12_RANGE noWrite{};
					pending->readback->Unmap(0,&noWrite);
					if (uint64_t(result.evaluated)+result.noHit+result.depthMismatch+result.missingMaterial+result.invalid+result.baseOnly+result.unsupportedMaterial+result.missingTextures==result.sampled) {
						stats=result;
					} else failure="Screen material counter mismatch";
				} else failure="Screen material counter readback failed";
			}
			pending.reset();
		}
	public:
		bool WantsFrame(uint32_t frame,bool continuous=false) const
		{
			bool free = false;
			for (const auto& pending : submissions) {
				if (!pending) free = true;
				else if (pending->value==UINT64_MAX) return false;
			}
			return free && (lastFrame==UINT32_MAX || frame-lastFrame>=(continuous?1u:30u));
		}
		void Dispatch(const GraphicsBackend& backend,std::shared_ptr<const RasterFrame> raster,
			std::shared_ptr<const TopLevel> scene,uint32_t frame,uint32_t width,uint32_t height,bool fullResolution,bool continuous=false,bool collectStats=false,bool parallaxEnabled=false,bool parallaxWarpFixEnabled=false,bool parallaxShadowEnabled=false,float mipBias=0.0f,std::shared_ptr<const ProjectedInputFrame> projection=nullptr,
			std::shared_ptr<const NativePBR::RawInputFrame> rawInputs=nullptr, uint32_t quality = kShadowQualityQuality,
			std::shared_ptr<Util::RuntimeTaskGate> taskGate=nullptr)
		{
			if (!WantsFrame(frame,continuous)) return;
			published.reset();
			auto availableSlot = std::find_if(submissions.begin(), submissions.end(), [](const auto& entry) { return !entry; });
			if (availableSlot==submissions.end()) return;
			auto& pending = *availableSlot;
			if (!raster || raster->frame != frame || !raster->depth || !raster->depth->resource || !raster->cameraValid || !raster->readyFence ||
				!MaterialFrameContract::ValidProducerSignal(raster->readyValue, raster->readyFence->GetCompletedValue()) ||
				!width || !height || width>raster->width || height>raster->height) {
				failure="Waiting for current raster depth"; return;
			}
			if (fullResolution && (!rawInputs || !rawInputs->Matches({frame, rawInputs->stamp.scope, 0, 0}, width, height) ||
				!rawInputs->evidence->winnerCertified)) {
				failure="Waiting for certified raster material inputs"; return;
			}
			// A stale or unprepared scene is optional. Never reject a primary
			// raster frame on the basis of geometry or scene material revisions.
			if (scene && (!scene->scope || !scene->acceleration || !scene->producerFence ||
				!MaterialFrameContract::ValidProducerSignal(scene->producerValue, scene->producerFence->GetCompletedValue()) ||
				!scene->shadingGeometry || !scene->shadingGeometry->buffer || scene->shadingGeometry->revision != scene->geometryRevision ||
				!scene->materialEvaluation || !scene->materialEvaluation->records || !scene->materialEvaluation->bindings ||
				scene->materialEvaluation->instanceCount != scene->shadingGeometry->instanceCount ||
				(rawInputs && (scene->scope != rawInputs->stamp.scope || scene->geometryRevision != rawInputs->stamp.geometryRevision))))
				scene.reset();
			if (!fullResolution && !scene) { failure="No secondary scene for diagnostic probe"; return; }
			bool submitted=false;
			std::shared_ptr<RasterPreparation> preparedJob;
			try {
				Initialize(backend);
				auto* device=backend.d3d12Device.get();
				const auto catalog=NativePBR::MaterialCatalog::Shared().GetSnapshot();
				const auto desiredSeed=scene ? scene->materialEvaluation : nullptr;
				std::shared_ptr<const Eval> selectedRaster;
				std::vector<uint32_t> rasterRemap;
				if (fullResolution) {
					const auto& sources=rawInputs->materialSourceSnapshots;
					if (sources.size()!=rawInputs->materialSources.size()) {
						failure="Incoherent raster material source rows"; return;
					}
					const auto scope=rawInputs->stamp.scope;
					if (rasterJob && rasterJob->finished.load(std::memory_order_acquire)) {
						if (rasterJob->scope!=scope || rasterJob->catalog!=catalog) rasterJob.reset();
						else if (!rasterJob->result) {
							failure=rasterJob->failure; rasterJob.reset(); lastRasterFailure=frame;
						} else preparedJob=rasterJob;
					}
					if (preparedJob) {
						selectedRaster=preparedJob->result;
						rasterRemap=MapRasterSources(sources,preparedJob->sources);
					} else if (rasterCache && rasterCacheScope==scope && rasterCatalog==catalog) {
						selectedRaster=rasterCache;
						rasterRemap=MapRasterSources(sources,rasterSources);
					}
					if (!selectedRaster || !CompatibleTextureNamespace(selectedRaster.get(),desiredSeed.get()) ||
						[&] {
								for (size_t i=0; i<sources.size(); ++i)
									if (sources[i] && rasterRemap[i]==UINT32_MAX) return true;
								return false;
							}())
						QueueRasterPreparation(backend,taskGate,scope,frame,sources,desiredSeed,catalog);
					if (!selectedRaster) { failure="Waiting for asynchronous raster material residency"; return; }
					// A newer scene may need a different descriptor prefix.
					// Retain primary material authority with its old compatible
					// texture namespace; enable new secondary inputs once prepared.
					if (!CompatibleTextureNamespace(selectedRaster.get(),desiredSeed.get())) scene.reset();
				}
				const auto seed=scene ? scene->materialEvaluation : nullptr;
				const Eval emptyEvaluation{};
				const auto& evaluation=seed ? *seed : emptyEvaluation;
				const bool mappedScene=scene && rawInputs && rawInputs->MatchesRayGeneration(
					{frame,scene->scope,scene->geometryRevision,evaluation.revision});
				const uint32_t gridWidth=fullResolution?width:kWidth, gridHeight=fullResolution?height:kHeight;
				const uint64_t count=uint64_t(gridWidth)*gridHeight;
				if (count>UINT32_MAX || gridWidth>16384 || gridHeight>16384) { failure="Screen material dimensions unsupported"; return; }
				const uint32_t pitchPixels=(gridWidth+7u)&~7u;
				const uint64_t bytes=uint64_t(pitchPixels)*gridHeight*kSampleStride;
				const uint64_t identityBytes=uint64_t(pitchPixels)*gridHeight*MaterialFrameContract::kRayIdentityStride;
				const bool shadowPlane=parallaxEnabled && parallaxShadowEnabled;
				const uint64_t lobeBytes=uint64_t(pitchPixels)*gridHeight*(MaterialFrameContract::kMaterialLobeStride +
					(shadowPlane ? MaterialFrameContract::kParallaxShadowStride : 0u));
				// ByteAddressBuffer uses 32-bit byte offsets.
				if (bytes>UINT32_MAX || lobeBytes>UINT32_MAX) { failure="Screen material byte address range exceeded"; return; }
				std::shared_ptr<OutputAllocation>* selected = nullptr;
				uint64_t tracked = 0;
				for (auto& entry : outputPool) {
					if (entry) tracked += entry->allocationBytes;
					if (!entry || entry.use_count() == 1) {
						if (!selected || (entry && entry->bytes == bytes && entry->lobeBytes == lobeBytes)) selected = &entry;
					}
				}
				if (!selected) { failure="Material frame pool busy"; return; }
				auto& reusableOutput = *selected;
				const bool reuse = reusableOutput && reusableOutput->bytes == bytes && reusableOutput->lobeBytes == lobeBytes;
				for (const auto& inFlight : submissions)
					if (inFlight) tracked += 6ull*65536;
				const auto available = videoBudget.Query(device, tracked);
				const auto desc=CD3DX12_RESOURCE_DESC::Buffer(bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				const auto identityDesc=CD3DX12_RESOURCE_DESC::Buffer(identityBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				const auto lobeDesc=CD3DX12_RESOURCE_DESC::Buffer(lobeBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				const auto sampleAllocation=device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
				const auto identityAllocation=device->GetResourceAllocationInfo(0,1,&identityDesc).SizeInBytes;
				const auto lobeAllocation=device->GetResourceAllocationInfo(0,1,&lobeDesc).SizeInBytes;
				if (sampleAllocation==UINT64_MAX || identityAllocation==UINT64_MAX || lobeAllocation==UINT64_MAX ||
					sampleAllocation>UINT64_MAX-identityAllocation || sampleAllocation+identityAllocation>UINT64_MAX-lobeAllocation) {
					failure="Screen material/identity allocation unsupported"; return;
				}
				const auto allocation=reuse?0:sampleAllocation+identityAllocation+lobeAllocation;
				if (!available.localBudget || allocation==UINT64_MAX || tracked>available.effectiveBytes || allocation>available.effectiveBytes-tracked || available.effectiveBytes-tracked-allocation<6ull*65536) {
					failure="Screen probe deferred by DXGI budget"; return;
				}
				auto slot=std::make_shared<Submission>();
				slot->raster=std::move(raster); slot->scene=std::move(scene);
				slot->scope = rawInputs ? rawInputs->stamp.scope : slot->scene->scope;
				slot->geometryRevision = rawInputs ? rawInputs->stamp.geometryRevision : slot->scene->geometryRevision;
				if (rawInputs && projection && projection->stamp == rawInputs->stamp &&
					projection->rowCount==projection->instances.size() && projection->rowCount==projection->rasterDraws.size())
					slot->projection=std::move(projection);
				if (fullResolution) slot->rawInputs=std::move(rawInputs);
				slot->rasterEvaluation=selectedRaster;
				if (preparedJob) slot->rasterUploads=preparedJob->uploads;
				if (rasterRemap.empty()) rasterRemap.push_back(UINT32_MAX);
				slot->rasterMaterialLookup=Eval::Table(device,rasterRemap);
				// Valid backing for disabled buffer root SRVs; all corresponding
				// shader counts are zero. Acceleration SRVs use a null descriptor.
				slot->emptyTable = Eval::Table(device, std::vector<uint32_t>(128u, 0u));
				std::vector<uint32_t> projectionLookup(std::max(1u,evaluation.instanceCount),UINT32_MAX);
				std::vector<uint32_t> rasterProjectionLookup(slot->rawInputs ? std::max<size_t>(1u,slot->rawInputs->drawRecords.size()) : 1u,UINT32_MAX);
				if (slot->projection)
					for (uint32_t row=0;row<slot->projection->rowCount;++row) {
						const auto instance=slot->projection->instances[row];
						if (mappedScene && instance<evaluation.instanceCount) projectionLookup[instance]=row;
						const auto draw=slot->projection->rasterDraws[row];
						if (slot->rawInputs && draw && draw<=slot->rawInputs->drawRecords.size())
							rasterProjectionLookup[draw-1u]=row;
					}
				slot->projectionLookup=Eval::Table(device,projectionLookup);
				slot->rasterProjectionLookup=Eval::Table(device,rasterProjectionLookup);
				// TLAS instance ordinals are the order of SceneInputs::geometry.
				// Cache the local-to-world rows rather than uploading them every frame.
				if (transformScene.lock() != slot->scene || !transformTable) {
					std::vector<std::array<float, 12>> rows;
					if (slot->scene && slot->scene->inputs && slot->scene->inputs->geometry.size() == evaluation.instanceCount) {
						rows.reserve(evaluation.instanceCount);
						for (const auto& shape : slot->scene->inputs->geometry) {
							if (!shape) { rows.clear(); break; }
							rows.push_back(shape->transform);
						}
					}
					transformCount = uint32_t(rows.size());
					if (rows.empty()) rows.emplace_back();
					transformTable = Eval::Table(device, rows);
					transformScene = slot->scene;
				}
				slot->transforms = transformTable;
				slot->frame=frame; slot->width=gridWidth; slot->height=gridHeight; slot->fullResolution=fullResolution; slot->collectStats=collectStats; slot->fence=fence; slot->root=root; slot->pipeline=pipeline;
				if (!reuse) {
					auto replacement=std::make_shared<OutputAllocation>();
					replacement->resource=Eval::Buffer(device,bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
					replacement->rayIdentity=Eval::Buffer(device,identityBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
					replacement->bytes=bytes;
					replacement->identityBytes=identityBytes;
					replacement->materialLobes=Eval::Buffer(device,lobeBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
					replacement->lobeBytes=lobeBytes;
					replacement->allocationBytes=allocation;
					reusableOutput=std::move(replacement);
				}
				slot->output=reusableOutput;
				slot->counters=Eval::Buffer(device,kCounterBytes,D3D12_HEAP_TYPE_DEFAULT,
					collectStats ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				if (collectStats) {
					slot->readback=Eval::Buffer(device,kCounterBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
					slot->counterInit=Eval::Table(device,std::vector<uint32_t>(kCounterCount,0));
				}
				slot->camera=Eval::Buffer(device,256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
				const Camera constants{slot->raster->camera.CameraProjInverse,slot->raster->camera.CameraViewInverse,
					slot->raster->camera.CameraPosAdjust,width,height,gridWidth,gridHeight,collectStats ? 1u : 0u,parallaxEnabled ? 1u : 0u,parallaxWarpFixEnabled ? 1u : 0u,shadowPlane ? 1u : 0u,std::isfinite(mipBias) ? mipBias : 0.0f,slot->projection ? slot->projection->rowCount : 0u,frame,slot->rawInputs ? slot->rawInputs->ConsumerDrawCount({frame,slot->scope,mappedScene ? slot->scene->geometryRevision : 0u,mappedScene ? evaluation.revision : 0u}) : 0u, SanitizeRTQuality(quality), transformCount,
					slot->rasterEvaluation ? slot->rasterEvaluation->instanceCount : 0u,
					slot->rasterEvaluation ? slot->rasterEvaluation->recordCount : 0u, evaluation.recordCount,
					slot->rawInputs ? uint32_t(slot->rawInputs->materialSources.size()) : 0u,
					slot->scene ? 1u : 0u, mappedScene ? 1u : 0u,
					slot->rasterEvaluation ? slot->rasterEvaluation->rasterSourceCount : 0u};
				void* mapped=nullptr; const D3D12_RANGE noRead{};
				Eval::Check(slot->camera->Map(0,&noRead,&mapped));
				std::memcpy(mapped,&constants,sizeof(constants)); slot->camera->Unmap(0,nullptr);
				const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
					NativePBR::kGpuTextureDescriptorCapacity+4+NativePBR::InputOnly::kPlaneCount+kProjectedTextureCapacity,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
				Eval::Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(slot->descriptors.put())));
				const auto cpu=slot->descriptors->GetCPUDescriptorHandleForHeapStart();
				const auto step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				(slot->rasterEvaluation ? *slot->rasterEvaluation : evaluation).WriteTextureViews(device,cpu);
				D3D12_SHADER_RESOURCE_VIEW_DESC depth{};
				depth.Format=DXGI_FORMAT_R32_FLOAT; depth.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
				depth.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; depth.Texture2D.MipLevels=1;
				device->CreateShaderResourceView(slot->raster->depth->resource.get(),&depth,
					{cpu.ptr+SIZE_T(NativePBR::kGpuTextureDescriptorCapacity)*step});
				const auto projectionDescriptor=NativePBR::kGpuTextureDescriptorCapacity+1;
				WriteProjectionViews(device,slot->projection.get(),{cpu.ptr+SIZE_T(projectionDescriptor)*step});
				const auto rawDescriptor=projectionDescriptor+1+kProjectedTextureCapacity;
				D3D12_SHADER_RESOURCE_VIEW_DESC rawView{};
				rawView.Format=DXGI_FORMAT_R32G32B32A32_UINT;
				rawView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
				rawView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				rawView.Texture2D.MipLevels=1;
				for (uint32_t i=0;i<NativePBR::InputOnly::kPlaneCount;++i)
					device->CreateShaderResourceView(slot->rawInputs ? slot->rawInputs->planes[i]->resource.get() : nullptr,
						&rawView,{cpu.ptr+SIZE_T(rawDescriptor+i)*step});
				auto tokenView=depth;
				tokenView.Format=DXGI_FORMAT_R16G16B16A16_UNORM;
				device->CreateShaderResourceView(slot->rawInputs ? slot->rawInputs->evidence->texture->resource.get() : nullptr,
					&tokenView,{cpu.ptr+SIZE_T(rawDescriptor+NativePBR::InputOnly::kPlaneCount)*step});
				const auto sceneDescriptor=rawDescriptor+NativePBR::InputOnly::kPlaneCount+1u;
				D3D12_SHADER_RESOURCE_VIEW_DESC sceneView{};
				sceneView.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
				sceneView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				sceneView.RaytracingAccelerationStructure.Location=slot->scene ? slot->scene->acceleration->GetGPUVirtualAddress() : 0u;
				device->CreateShaderResourceView(nullptr,&sceneView,{cpu.ptr+SIZE_T(sceneDescriptor)*step});
				Eval::Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(slot->allocator.put())));
				Eval::Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot->allocator.get(),pipeline.get(),IID_PPV_ARGS(slot->commands.put())));
				auto* command=slot->commands.get();
				ID3D12DescriptorHeap* heaps[]{slot->descriptors.get()};
				command->SetDescriptorHeaps(1,heaps); command->SetComputeRootSignature(root.get());
				const auto emptyAddress=slot->emptyTable->GetGPUVirtualAddress();
				command->SetComputeRootShaderResourceView(0,slot->scene ? slot->scene->shadingGeometry->buffer->GetGPUVirtualAddress() : emptyAddress);
				command->SetComputeRootShaderResourceView(1,seed ? evaluation.records->GetGPUVirtualAddress() : emptyAddress);
				command->SetComputeRootShaderResourceView(2,seed ? evaluation.bindings->GetGPUVirtualAddress() : emptyAddress);
				command->SetComputeRootDescriptorTable(3,slot->descriptors->GetGPUDescriptorHandleForHeapStart());
				command->SetComputeRootUnorderedAccessView(4,slot->output->resource->GetGPUVirtualAddress());
				command->SetComputeRoot32BitConstant(5,evaluation.instanceCount,0);
				auto sceneGpu=slot->descriptors->GetGPUDescriptorHandleForHeapStart();
				sceneGpu.ptr+=UINT64(sceneDescriptor)*step;
				command->SetComputeRootDescriptorTable(6,sceneGpu);
				command->SetComputeRootConstantBufferView(7,slot->camera->GetGPUVirtualAddress());
				command->SetComputeRootUnorderedAccessView(8,slot->counters->GetGPUVirtualAddress());
				command->SetComputeRootUnorderedAccessView(9,slot->output->rayIdentity->GetGPUVirtualAddress());
				command->SetComputeRootUnorderedAccessView(10,slot->output->materialLobes->GetGPUVirtualAddress());
				command->SetComputeRootShaderResourceView(11,slot->projectionLookup->GetGPUVirtualAddress());
				auto projectedGpu=slot->descriptors->GetGPUDescriptorHandleForHeapStart();
				projectedGpu.ptr+=UINT64(projectionDescriptor)*step;
				command->SetComputeRootDescriptorTable(12,projectedGpu);
				auto rawGpu=slot->descriptors->GetGPUDescriptorHandleForHeapStart();
				rawGpu.ptr+=UINT64(rawDescriptor)*step;
				command->SetComputeRootDescriptorTable(13,rawGpu);
				command->SetComputeRootShaderResourceView(14,(slot->rawInputs ? slot->rawInputs->drawTable.get() : slot->projectionLookup.get())->GetGPUVirtualAddress());
				command->SetComputeRootShaderResourceView(15,slot->transforms->GetGPUVirtualAddress());
				command->SetComputeRootShaderResourceView(16,(slot->rasterEvaluation ? slot->rasterEvaluation->records : evaluation.records)->GetGPUVirtualAddress());
				command->SetComputeRootShaderResourceView(17,(slot->rasterEvaluation ? slot->rasterEvaluation->bindings : evaluation.bindings)->GetGPUVirtualAddress());
				command->SetComputeRootShaderResourceView(18,(slot->rasterEvaluation ? slot->rasterEvaluation->rasterLandscapeRows : slot->projectionLookup)->GetGPUVirtualAddress());
				command->SetComputeRootShaderResourceView(19,slot->rasterMaterialLookup->GetGPUVirtualAddress());
				command->SetComputeRootShaderResourceView(20,slot->rasterProjectionLookup->GetGPUVirtualAddress());
				Eval::RecordUploads(command, slot->rasterUploads);
				if (collectStats) {
					command->CopyBufferRegion(slot->counters.get(),0,slot->counterInit.get(),0,kCounterBytes);
					const auto counterBarrier=CD3DX12_RESOURCE_BARRIER::Transition(slot->counters.get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
					command->ResourceBarrier(1,&counterBarrier);
				}
				const auto begin=CD3DX12_RESOURCE_BARRIER::Transition(slot->raster->depth->resource.get(),
					D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				command->ResourceBarrier(1,&begin);
				TransitionProjectionViews(command, slot->projection.get(),
					D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				std::array<D3D12_RESOURCE_BARRIER,NativePBR::InputOnly::kPlaneCount+1> rawBarriers{};
				if (slot->rawInputs) {
					for (uint32_t i=0;i<NativePBR::InputOnly::kPlaneCount;++i)
						rawBarriers[i]=CD3DX12_RESOURCE_BARRIER::Transition(slot->rawInputs->planes[i]->resource.get(),
							D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
					rawBarriers.back()=CD3DX12_RESOURCE_BARRIER::Transition(slot->rawInputs->evidence->texture->resource.get(),
						D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
					command->ResourceBarrier(UINT(rawBarriers.size()),rawBarriers.data());
				}
				if (slot->output->shaderReadable) {
					const std::array outputBarriers{
						CD3DX12_RESOURCE_BARRIER::Transition(slot->output->resource.get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
						CD3DX12_RESOURCE_BARRIER::Transition(slot->output->rayIdentity.get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
						CD3DX12_RESOURCE_BARRIER::Transition(slot->output->materialLobes.get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
					command->ResourceBarrier(UINT(outputBarriers.size()),outputBarriers.data());
				}
				command->Dispatch((gridWidth+7)/8,(gridHeight+7)/8,1);
				if (slot->rawInputs) {
					for (auto& barrier:rawBarriers) std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
					command->ResourceBarrier(UINT(rawBarriers.size()),rawBarriers.data());
				}
				TransitionProjectionViews(command, slot->projection.get(),
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
				const std::array end{
					CD3DX12_RESOURCE_BARRIER::Transition(slot->output->resource.get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON),
					CD3DX12_RESOURCE_BARRIER::Transition(slot->output->rayIdentity.get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON),
					CD3DX12_RESOURCE_BARRIER::Transition(slot->output->materialLobes.get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON),
					CD3DX12_RESOURCE_BARRIER::Transition(slot->raster->depth->resource.get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON)};
				command->ResourceBarrier(UINT(end.size()),end.data());
				if (collectStats) {
					const auto counterBarrier=CD3DX12_RESOURCE_BARRIER::Transition(slot->counters.get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
					command->ResourceBarrier(1,&counterBarrier);
					command->CopyBufferRegion(slot->readback.get(),0,slot->counters.get(),0,kCounterBytes);
				}
				Eval::Check(command->Close());
				Eval::Check(backend.commandQueue->Wait(slot->raster->readyFence.get(),slot->raster->readyValue));
				if (slot->scene) Eval::Check(backend.commandQueue->Wait(slot->scene->producerFence.get(),slot->scene->producerValue));
				if (slot->projection) Eval::Check(backend.commandQueue->Wait(slot->projection->readyFence.get(),slot->projection->readyValue));
				if (slot->rawInputs) {
					Eval::Check(backend.commandQueue->Wait(slot->rawInputs->readyFence.get(),slot->rawInputs->readyValue));
					Eval::Check(backend.commandQueue->Wait(slot->rawInputs->evidence->readyFence.get(),slot->rawInputs->evidence->readyValue));
				}
				pending=std::move(slot);
				ID3D12CommandList* lists[]{command}; backend.commandQueue->ExecuteCommandLists(1,lists);
				submitted=true;
				lastFrame=frame;
				pending->output->shaderReadable=true;
				++submittedFrames;
				if (reuse) ++reusedAllocations;
				const auto value=nextValue+1;
				Eval::Check(backend.commandQueue->Signal(fence.get(),value));
				nextValue=value; pending->value=value;
				if (preparedJob) {
					rasterCache=pending->rasterEvaluation;
					rasterCatalog=preparedJob->catalog;
					rasterCacheScope=preparedJob->scope;
					rasterSources=std::move(preparedJob->sources);
					rasterJob.reset();
					lastRasterFailure=UINT32_MAX;
				}
				if (pending->fullResolution) {
					auto materialFrame=std::make_shared<MaterialFrame>();
					materialFrame->allocation=pending->output; materialFrame->samples=pending->output->resource; materialFrame->producerFence=pending->fence;
					materialFrame->rayIdentity=pending->output->rayIdentity;
					materialFrame->rayIdentityRowPitch=pitchPixels*MaterialFrameContract::kRayIdentityStride;
					materialFrame->materialLobes=pending->output->materialLobes;
					materialFrame->materialLobeRowPitch=pitchPixels*MaterialFrameContract::kMaterialLobeStride;
					materialFrame->parallaxEnabled=parallaxEnabled;
					materialFrame->parallaxShadows=shadowPlane;
					materialFrame->rawInputDraws=pending->rawInputs ? pending->rawInputs->draws : 0u;
					materialFrame->rawRasterEvidence=pending->rawInputs;
					materialFrame->rasterMaterialEvaluation=pending->rasterEvaluation;
					materialFrame->rasterMaterialRevision=pending->rasterEvaluation ? pending->rasterEvaluation->revision : 0;
					materialFrame->rasterMaterialLookup=pending->rasterMaterialLookup;
					materialFrame->rasterMaterialSourceCount=pending->rawInputs ? uint32_t(pending->rawInputs->materialSources.size()) : 0u;
					materialFrame->rasterUnpreparedSources=uint32_t(std::count(rasterRemap.begin(),rasterRemap.end(),UINT32_MAX));
					materialFrame->projection=pending->projection;
					materialFrame->projectionLookup=pending->projectionLookup;
					materialFrame->raster=pending->raster; materialFrame->scene=pending->scene;
					materialFrame->descriptors=pending->descriptors;
					materialFrame->producerValue=value; materialFrame->geometryRevision=pending->geometryRevision;
					materialFrame->materialRevision=seed ? seed->revision : 0;
					materialFrame->frame=frame; materialFrame->scope=pending->scope;
					materialFrame->width=gridWidth; materialFrame->height=gridHeight; materialFrame->rowPitch=pitchPixels*kSampleStride;
					published=std::move(materialFrame);
				}
				failure.clear();
			} catch (const std::exception& error) {
				failure=error.what();
				published.reset();
				if (submitted && pending) {
					// Do not record upload commands twice after submission.
					if (preparedJob && rasterJob==preparedJob) rasterJob.reset();
					pending->discard=true;
					const auto rescue=nextValue+1;
					if (SUCCEEDED(backend.commandQueue->Signal(fence.get(),rescue))) {
						nextValue=rescue; pending->value=rescue;
					}
				} else pending.reset();
			}
		}
	};
}

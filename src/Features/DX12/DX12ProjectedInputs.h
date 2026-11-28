#pragma once

#include "DX12GpuRetirement.h"
#include "DX12MaterialFrameContract.h"
#include "GraphicsBackend.h"
#include "GraphicsInterop.h"
#include "VideoMemoryBudget.h"
#include "Utils/D3D.h"
#include "Utils/FileSystem.h"
#include <algorithm>
#include <array>
#include <memory>
#include <map>
#include <span>
#include <stdexcept>
#include <vector>
#include <DirectXTex.h>

namespace DX12Native
{
	// The capture budget is shared with the existing bounded draw snapshot pool.
	inline constexpr uint32_t kMaxProjectedDraws = 128;
	inline constexpr uint32_t kProjectedTextureRoles = 4;
	inline constexpr uint32_t kProjectedTextureCapacity = kMaxProjectedDraws * kProjectedTextureRoles;
	inline constexpr uint32_t kProjectedRecordColumns = 8;
	// Reserved non-scene row IDs in the existing private transfer record.
	inline constexpr uint32_t kRasterGrassRow = UINT32_MAX - 1u;
	inline constexpr uint32_t kRasterDistantTreeRow = UINT32_MAX - 2u;
	inline constexpr uint32_t kRasterEffectRow = UINT32_MAX - 3u;
	struct ProjectedInputDraw
	{
		uint32_t instance = UINT32_MAX, descriptor = 0, samplerModes = 0;
		std::array<ID3D11ShaderResourceView*, 2> constants{};
		std::array<uint32_t, 4> offsets{};
		std::array<winrt::com_ptr<ID3D11ShaderResourceView>, kProjectedTextureRoles> views;
		uint32_t rasterDraw = 0;
		std::vector<uint32_t> rasterAliases;
	};
	struct ProjectedInputFrame
	{
		struct Texture
		{
			winrt::com_ptr<ID3D11ShaderResourceView> source;
			winrt::com_ptr<ID3D12Resource> resource;
			std::shared_ptr<WrappedResource> bridge;
			D3D11_SHADER_RESOURCE_VIEW_DESC view{};
			uint64_t bytes = 0, nativeBytes = 0;
			bool immutable = false;
			bool uploaded = false;
		};
		MaterialFrameContract::RasterIdentityStamp stamp{};
		std::unique_ptr<WrappedResource> records;
		std::vector<Texture> textures;
		std::vector<uint32_t> instances, rasterDraws;
		std::vector<std::pair<uint32_t, uint32_t>> rasterAliases; // draw ordinal, transfer row
		uint32_t rowCount = 0, missingTextures = 0;
		uint32_t transferredTextures = 0, newTextures = 0, cacheHits = 0, suppressedTextures = 0, deferredTextures = 0;
		uint64_t immutableCacheBytes = 0;
		uint64_t allocationBytes = 0, recordBytes = 0, readyValue = 0;
		winrt::com_ptr<ID3D12Fence> readyFence;
		winrt::com_ptr<ID3D12CommandAllocator> transferAllocator;
		winrt::com_ptr<ID3D12GraphicsCommandList> transferCommands;
		bool Ready(const MaterialFrameContract::RasterIdentityStamp& requested) const
		{
			return stamp == requested && rowCount && records && records->resource && readyFence &&
				MaterialFrameContract::ValidProducerSignal(readyValue, readyFence->GetCompletedValue());
		}
	};

	// Recreate typed views; never copy from a shader-visible descriptor heap.
    inline void WriteProjectionViews(ID3D12Device* device,const ProjectedInputFrame* frame,D3D12_CPU_DESCRIPTOR_HANDLE start)
    {
        const auto step=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format=DXGI_FORMAT_R32G32B32A32_UINT;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(frame?frame->records->resource.get():nullptr,&view,start);
        for(uint32_t index=0;index<kProjectedTextureCapacity;++index){
            view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;view.Texture2D.MostDetailedMip=0;view.Texture2D.MipLevels=1;
            ID3D12Resource* resource=nullptr;
            if(frame&&index<frame->textures.size()){
                const auto& texture=frame->textures[index];
                view.Format=texture.view.Format;view.Texture2D.MostDetailedMip=texture.view.Texture2D.MostDetailedMip;
                view.Texture2D.MipLevels=texture.view.Texture2D.MipLevels;resource=texture.resource.get();
            }
            device->CreateShaderResourceView(resource,&view,{start.ptr+SIZE_T(index+1)*step});
        }
    }
    inline void TransitionProjectionViews(ID3D12GraphicsCommandList* cmd,const ProjectedInputFrame* frame,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to)
    {
        if(!frame)return;
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        barriers.reserve(frame->textures.size()+1);
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(frame->records->resource.get(),from,to));
        for(const auto& texture:frame->textures)
            barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(texture.resource.get(),from,to));
        cmd->ResourceBarrier(UINT(barriers.size()),barriers.data());
    }

    /** @brief GPU-only projection publication. No readback, waits on CPU, or changes to raster lighting. */
	class ProjectedInputCapture
	{
		struct alignas(16) PackConstants
		{
			std::array<uint32_t, 4> offsets;
			std::array<uint32_t, 4> metadata;
			std::array<uint32_t, 4> textures;
		};
		std::array<std::shared_ptr<ProjectedInputFrame>, 3> slots;
		std::shared_ptr<const ProjectedInputFrame> current;
		GraphicsBackend owner;
		SharedFence producer;
		VideoMemoryBudget budget;
		winrt::com_ptr<ID3D11ComputeShader> packShader;
		winrt::com_ptr<ID3D11Buffer> packConstants;
		winrt::com_ptr<ID3D11ComputeShader> decodeShader;
		winrt::com_ptr<ID3D11Buffer> decodeConstants;
		struct alignas(16) DecodeConstants { uint32_t width, height, offsetY, mip; };
		bool failureLogged = false;
		winrt::com_ptr<ID3D12Fence> transferFence;
		uint64_t transferValue = 0;
		static constexpr size_t kMaxFailedDescriptors = 256;
		static constexpr size_t kMaxCachedTextures = 256;
		static constexpr uint64_t kCacheBudget = 256ull * VideoMemoryBudget::kMiB;
		static constexpr uint32_t kMaxNewTextures = 16;
		static constexpr uint64_t kNewBytesPerPublication = 128ull * VideoMemoryBudget::kMiB;
		using FailureKey = std::array<uint32_t, 5>;
		static constexpr uint32_t kFailureCooldown = 600;
		std::map<FailureKey, uint32_t> failedDescriptors;
		struct CachedTexture {
			ProjectedInputFrame::Texture texture;
			uint32_t lastFrame = 0;
		};
		std::vector<CachedTexture> immutableCache;
		std::map<ID3D11ShaderResourceView*, uint32_t> frameImports;
		uint32_t transferred = 0, allocated = 0;
		uint64_t cacheBytes = 0, newBytes = 0;
		struct MipTransfer {
			uint32_t width, height, y;
		};
		// Decode through the actual typed DX11 SRV. No cross-format copy or
		// compressed mip-tail reinterpretation is legal in this path.
		static DXGI_FORMAT BridgeFormat(DXGI_FORMAT format)
		{
			switch (format) {
			case DXGI_FORMAT_BC1_UNORM:
			case DXGI_FORMAT_BC2_UNORM:
			case DXGI_FORMAT_BC3_UNORM:
			case DXGI_FORMAT_BC4_UNORM:
			case DXGI_FORMAT_BC5_UNORM:
			case DXGI_FORMAT_BC7_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8X8_UNORM:
			case DXGI_FORMAT_R8_UNORM:
			case DXGI_FORMAT_R8G8_UNORM:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			default:
				// sRGB is decoded to linear by the source SRV. Signed and
				// HDR sources must not be clamped to UNORM.
				return DXGI_FORMAT_R16G16B16A16_FLOAT;
			}
		}
		bool SupportsBridge(DXGI_FORMAT format) const
		{
			UINT support = 0;
			D3D11_FEATURE_DATA_FORMAT_SUPPORT2 extended{ format, 0 };
			D3D12_FEATURE_DATA_FORMAT_SUPPORT native{ format };
			return SUCCEEDED(owner.d3d11Device->CheckFormatSupport(format, &support)) &&
				(support & D3D11_FORMAT_SUPPORT_TEXTURE2D) &&
				(support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) &&
				SUCCEEDED(owner.d3d11Device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &extended, sizeof(extended))) &&
				(extended.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_SHAREABLE) &&
				(extended.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) &&
				SUCCEEDED(owner.d3d12Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &native, sizeof(native))) &&
				(native.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) &&
				(native.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE);
		}
		void DecodeMipChain(ID3D11ShaderResourceView* view, const ProjectedInputFrame::Texture& output,
			std::span<const MipTransfer> mips)
		{
			auto* ctx = owner.d3d11Context.get();
			winrt::com_ptr<ID3D11ComputeShader> priorShader;
			winrt::com_ptr<ID3D11ShaderResourceView> priorRead;
			winrt::com_ptr<ID3D11UnorderedAccessView> priorWrite;
			winrt::com_ptr<ID3D11Buffer> priorConstants;
			ctx->CSGetShader(priorShader.put(), nullptr, nullptr);
			ctx->CSGetShaderResources(0, 1, priorRead.put());
			ctx->CSGetUnorderedAccessViews(0, 1, priorWrite.put());
			ctx->CSGetConstantBuffers(0, 1, priorConstants.put());
			auto* write = output.bridge->uav;
			auto* constants = decodeConstants.get();
			ctx->CSSetShader(decodeShader.get(), nullptr, 0);
			ctx->CSSetShaderResources(0, 1, &view);
			ctx->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
			ctx->CSSetConstantBuffers(0, 1, &constants);
			for (uint32_t mip = 0; mip < mips.size(); ++mip) {
				const auto& region = mips[mip];
				const DecodeConstants data{ region.width, region.height, region.y, mip };
				ctx->UpdateSubresource(constants, 0, nullptr, &data, 0, 0);
				ctx->Dispatch((region.width + 7u) / 8u, (region.height + 7u) / 8u, 1);
			}
			write = priorWrite.get();
			ctx->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
			auto* read = priorRead.get();
			ctx->CSSetShaderResources(0, 1, &read);
			constants = priorConstants.get();
			ctx->CSSetConstantBuffers(0, 1, &constants);
			ctx->CSSetShader(priorShader.get(), nullptr, 0);
		}
		void TrimCache(uint32_t frame)
		{
			std::erase_if(immutableCache, [&](const CachedTexture& entry) {
				if (frame - entry.lastFrame <= 120u) return false;
				cacheBytes -= entry.texture.nativeBytes;
				return true;
			});
		}
		void CacheSubmitted(const ProjectedInputFrame& frame)
		{
			for (const auto& texture : frame.textures) {
				if (!texture.immutable || !texture.uploaded ||
					immutableCache.size() >= kMaxCachedTextures || texture.nativeBytes > kCacheBudget - cacheBytes)
					continue;
				if (std::ranges::any_of(immutableCache, [&](const auto& item) {
					return item.texture.source.get() == texture.source.get();
				})) continue;
				auto copy = texture;
				copy.bridge.reset(); copy.bytes = copy.nativeBytes; copy.uploaded = false;
				immutableCache.push_back({ std::move(copy), frame.stamp.frame });
				cacheBytes += texture.nativeBytes;
			}
		}

		uint64_t Allocation(const D3D12_RESOURCE_DESC& desc)
		{
			const auto allocation = owner.d3d12Device->GetResourceAllocationInfo(0, 1, &desc);
			const auto allowance = budget.Query(owner.d3d12Device.get(), 0).effectiveBytes;
			if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX || allocation.SizeInBytes > allowance)
				return 0;
			return allocation.SizeInBytes;
		}
		uint32_t ImportTexture(ProjectedInputFrame& frame, std::vector<ProjectedInputFrame::Texture>& reusable, ID3D11ShaderResourceView* view)
		{
			if (!view) return UINT32_MAX;
			// Cache negative results for the current publication too.
			auto [known, inserted] = frameImports.try_emplace(view, UINT32_MAX);
			if (!inserted) return known->second;
			if (frame.textures.size() >= kProjectedTextureCapacity) return UINT32_MAX;
			const auto publish = [&](ProjectedInputFrame::Texture entry) {
				const auto index = static_cast<uint32_t>(frame.textures.size());
				frame.textures.push_back(std::move(entry));
				known->second = index;
				return index;
			};
			D3D11_SHADER_RESOURCE_VIEW_DESC sourceView{};
			view->GetDesc(&sourceView);
			winrt::com_ptr<ID3D11Resource> source;
			view->GetResource(source.put());
			auto texture = source.try_as<ID3D11Texture2D>();
			D3D11_TEXTURE2D_DESC desc{};
			if (texture) texture->GetDesc(&desc);
			if (!texture || sourceView.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
				sourceView.Format == DXGI_FORMAT_UNKNOWN || DirectX::MakeTypeless(sourceView.Format) != DirectX::MakeTypeless(desc.Format) ||
				!desc.Width || !desc.Height || !desc.MipLevels || desc.ArraySize != 1 ||
				desc.SampleDesc.Count != 1 || desc.CPUAccessFlags ||
				(desc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_DEPTH_STENCIL)))
				return UINT32_MAX;
			const auto firstMip = sourceView.Texture2D.MostDetailedMip;
			if (firstMip >= desc.MipLevels) return UINT32_MAX;
			auto& levels = sourceView.Texture2D.MipLevels;
			if (levels == UINT32_MAX) levels = desc.MipLevels - firstMip;
			if (!levels || levels > desc.MipLevels - firstMip) return UINT32_MAX;
			const bool immutable = desc.Usage == D3D11_USAGE_IMMUTABLE;
			if (immutable) {
				const auto cached = std::ranges::find_if(immutableCache, [view](const auto& item) { return item.texture.source.get() == view; });
				if (cached != immutableCache.end()) {
					cached->lastFrame = frame.stamp.frame;
					++frame.cacheHits;
					frame.allocationBytes += cached->texture.bytes;
					return publish(cached->texture);
				}
			}
			const FailureKey key{ uint32_t(sourceView.Format), desc.Width, desc.Height, firstMip, levels };
			if (const auto rejected = failedDescriptors.find(key); rejected != failedDescriptors.end()) {
				if (!MaterialFrameContract::ProjectionRetryDue(frame.stamp.frame, rejected->second, kFailureCooldown)) {
					++frame.suppressedTextures;
					return UINT32_MAX;
				}
				failedDescriptors.erase(rejected);
			}
			if (failedDescriptors.size() >= kMaxFailedDescriptors) { ++frame.deferredTextures; return UINT32_MAX; }
			D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ sourceView.Format };
			if (FAILED(owner.d3d12Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
				!(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) ||
				!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D))
				return UINT32_MAX;
			UINT sourceSupport = 0;
			if (FAILED(owner.d3d11Device->CheckFormatSupport(sourceView.Format, &sourceSupport)) ||
				!(sourceSupport & D3D11_FORMAT_SUPPORT_SHADER_LOAD) || !(sourceSupport & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))
				return UINT32_MAX;
			const DXGI_FORMAT bridgeFormat = BridgeFormat(sourceView.Format);
			if (!SupportsBridge(bridgeFormat)) {
				failedDescriptors.insert_or_assign(key, frame.stamp.frame);
				++frame.suppressedTextures;
				logger::warn("[DX12NativeScene] Projection decode bridge format {} is not shareable/typed-store capable", uint32_t(bridgeFormat));
				return UINT32_MAX;
			}
			// Copy only the visible SRV mip range. Native mip zero corresponds
			// to the original view's MostDetailedMip, including its dimensions.
			const auto baseWidth = MaterialFrameContract::ProjectionTransferExtent(desc.Width, firstMip, false);
			const auto baseHeight = MaterialFrameContract::ProjectionTransferExtent(desc.Height, firstMip, false);
			std::vector<MipTransfer> mips;
			mips.reserve(levels);
			uint32_t atlasWidth = 0, atlasHeight = 0;
			for (uint32_t mip = 0; mip < levels; ++mip) {
				const auto width = MaterialFrameContract::ProjectionTransferExtent(baseWidth, mip, false);
				const auto height = MaterialFrameContract::ProjectionTransferExtent(baseHeight, mip, false);
				mips.push_back({ width, height, atlasHeight });
				atlasWidth = std::max(atlasWidth, width);
				atlasHeight += height;
			}
			if (atlasWidth > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || atlasHeight > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
				return UINT32_MAX;
			ProjectedInputFrame::Texture entry;
			const auto old = std::ranges::find_if(reusable, [view](const auto& item) {
				return item.source.get() == view && item.resource && (item.immutable || item.bridge);
			});
			if (old != reusable.end()) {
				entry = std::move(*old);
				old->bytes = 0;
				if (entry.immutable && !entry.bridge) { entry.uploaded = false; return publish(std::move(entry)); }
			} else {
				if (allocated >= kMaxNewTextures) { ++frame.deferredTextures; return UINT32_MAX; }
				const auto nativeDesc = CD3DX12_RESOURCE_DESC::Tex2D(bridgeFormat, baseWidth, baseHeight, 1, static_cast<UINT16>(levels));
				const auto bridgeDesc = CD3DX12_RESOURCE_DESC::Tex2D(bridgeFormat, atlasWidth, atlasHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
				const uint64_t nativeBytes = Allocation(nativeDesc), bridgeBytes = Allocation(bridgeDesc);
				const auto allowance = budget.Query(owner.d3d12Device.get(), 0).effectiveBytes;
				if (!nativeBytes || !bridgeBytes || nativeBytes > allowance || bridgeBytes > allowance - nativeBytes ||
					(allocated && (newBytes >= kNewBytesPerPublication || nativeBytes + bridgeBytes > kNewBytesPerPublication - newBytes))) {
					++frame.deferredTextures;
					return UINT32_MAX;
				}
				try {
					const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
					DX::ThrowIfFailed(owner.d3d12Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &nativeDesc,
						D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(entry.resource.put())));
					D3D11_TEXTURE2D_DESC shared{};
					shared.Width = atlasWidth; shared.Height = atlasHeight;
					shared.MipLevels = shared.ArraySize = 1; shared.Format = bridgeFormat;
					shared.SampleDesc = { 1, 0 }; shared.Usage = D3D11_USAGE_DEFAULT;
					shared.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
					entry.bridge = std::make_shared<WrappedResource>(shared, owner.d3d11Device.get(), owner.d3d12Device.get(),
						"DX12NativeScene::ProjectedMipBridge");
				} catch (...) {
					failedDescriptors.insert_or_assign(key, frame.stamp.frame);
					logger::warn("[DX12NativeScene] Projection transfer rejected: format {}, {}x{}, {} mips; descriptor cooled down for 600 frames",
						uint32_t(sourceView.Format), desc.Width, desc.Height, desc.MipLevels);
					return UINT32_MAX;
				}
				entry.source.copy_from(view);
				entry.view = sourceView; entry.view.Format = bridgeFormat;
				entry.view.Texture2D.MostDetailedMip = 0; entry.immutable = immutable;
				entry.nativeBytes = nativeBytes; entry.bytes = nativeBytes + bridgeBytes;
				frame.allocationBytes += entry.bytes;
				++allocated;
				newBytes += entry.bytes;
			}
			// Retain resources in the slot before recording either API's work.
			entry.uploaded = true;
			const auto index = publish(std::move(entry));
			const auto& output = frame.textures[index];
			DecodeMipChain(view, output, mips);
			auto* cmd = frame.transferCommands.get();
			const D3D12_RESOURCE_BARRIER begin[]{
				CD3DX12_RESOURCE_BARRIER::Transition(output.bridge->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(output.resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)
			};
			cmd->ResourceBarrier(UINT(std::size(begin)), begin);
			for (uint32_t mip = 0; mip < mips.size(); ++mip) {
				const auto& region = mips[mip];
				const CD3DX12_TEXTURE_COPY_LOCATION src(output.bridge->resource.get(), 0u);
				const CD3DX12_TEXTURE_COPY_LOCATION dst(output.resource.get(), mip);
				const D3D12_BOX box{ 0, region.y, 0, region.width, region.y + region.height, 1 };
				cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
			}
			const D3D12_RESOURCE_BARRIER finish[]{
				CD3DX12_RESOURCE_BARRIER::Transition(output.bridge->resource.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(output.resource.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)
			};
			cmd->ResourceBarrier(UINT(std::size(finish)), finish);
			++transferred;
			return index;
		}

	public:
		~ProjectedInputCapture()
		{
			current.reset();
			for (auto& slot : slots) if (slot) {
				auto dependency = FencePoint(slot->readyFence.get(), slot->readyValue);
				RetireAfterFences(std::move(slot), owner.d3d12Device, { std::move(dependency) });
			}
		}
		void Reset() { current.reset(); failureLogged = false; immutableCache.clear(); cacheBytes = 0; frameImports.clear(); }
		std::shared_ptr<const ProjectedInputFrame> Get(const MaterialFrameContract::RasterIdentityStamp& stamp) const
		{
			return current && current->Ready(stamp) ? current : nullptr;
		}
		std::shared_ptr<const ProjectedInputFrame> GetSubmitted(const MaterialFrameContract::RasterIdentityStamp& stamp) const
		{
			return current && current->stamp == stamp && current->rowCount && current->readyFence &&
				current->readyValue && current->readyValue != UINT64_MAX ? current : nullptr;
		}
		void Publish(const GraphicsBackend& backend, MaterialFrameContract::RasterIdentityStamp stamp, std::span<const ProjectedInputDraw> draws)
		{
			current.reset();
			if (!GraphicsBackend::CheckNativeDeviceHealth(backend.d3d12Device.get(), backend.d3d11Device.get())) return;
			if (draws.empty() || draws.size() > kMaxProjectedDraws || !stamp.scope)
				return;
			std::shared_ptr<ProjectedInputFrame> pending;
			bool submitted = false, transferSubmitted = false;
			try {
				if (!producer.fence12) {
					owner = backend;
					DX::ThrowIfFailed(owner.d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(transferFence.put())));
					producer.Create(owner.d3d12Device.get(), owner.d3d11Device.get(), "DX12NativeScene::ProjectionReady");
				}
				if (owner.d3d12Device.get() != backend.d3d12Device.get() || owner.d3d11Context.get() != backend.d3d11Context.get() ||
					owner.commandQueue.get() != backend.commandQueue.get()) return;
				auto free = std::find_if(slots.begin(), slots.end(), [](const auto& slot) {
					return !slot || (slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue));
				});
				if (free == slots.end()) return;
				if (!packShader) {
					const auto path = Util::PathHelpers::GetShadersPath() / "DX12NativeScene" / "PackProjectedInputs.cs.hlsl";
					packShader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), {}, "cs_5_0")));
				}
				if (!packShader) return;
				if (!decodeShader) {
					const auto path = Util::PathHelpers::GetShadersPath() / "DX12NativeScene" / "DecodeProjectedMip.cs.hlsl";
					decodeShader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), {}, "cs_5_0")));
				}
				if (!decodeShader) return;
				if (!decodeConstants) {
					D3D11_BUFFER_DESC desc{};
					desc.ByteWidth = sizeof(DecodeConstants); desc.Usage = D3D11_USAGE_DEFAULT;
					desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
					DX::ThrowIfFailed(owner.d3d11Device->CreateBuffer(&desc, nullptr, decodeConstants.put()));
				}
				if (!packConstants) {
					D3D11_BUFFER_DESC desc{};
					desc.ByteWidth = sizeof(PackConstants); desc.Usage = D3D11_USAGE_DEFAULT;
					desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
					DX::ThrowIfFailed(owner.d3d11Device->CreateBuffer(&desc, nullptr, packConstants.put()));
				}
				// Reuse the small record image; imported textures are immutable for a
				// published frame and released only after its fence and borrowers retire.
				if (!*free) {
					D3D12_RESOURCE_DESC desc{};
					desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
					desc.Width = kProjectedRecordColumns; desc.Height = kMaxProjectedDraws;
					desc.DepthOrArraySize = 1; desc.MipLevels = 1;
					desc.Format = DXGI_FORMAT_R32G32B32A32_UINT; desc.SampleDesc = { 1, 0 };
					desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
					const auto bytes = Allocation(desc);
					if (!bytes) return;
					auto replacement = std::make_shared<ProjectedInputFrame>();
					D3D11_TEXTURE2D_DESC shared{};
					shared.Width = kProjectedRecordColumns; shared.Height = kMaxProjectedDraws;
					shared.MipLevels = shared.ArraySize = 1; shared.Format = desc.Format;
					shared.SampleDesc = { 1, 0 }; shared.Usage = D3D11_USAGE_DEFAULT;
					shared.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
					replacement->records = std::make_unique<WrappedResource>(shared, owner.d3d11Device.get(), owner.d3d12Device.get(), "DX12NativeScene::ProjectionRecords");
					replacement->allocationBytes = replacement->recordBytes = bytes;
					*free = std::move(replacement);
				}
				pending = *free;
				TrimCache(stamp.frame);
				std::erase_if(failedDescriptors, [&](const auto& entry) {
					return MaterialFrameContract::ProjectionRetryDue(stamp.frame, entry.second, kFailureCooldown);
				});
				frameImports.clear(); transferred = allocated = 0; newBytes = 0;
				// Slot selection already checks both GPU completion and borrowers.
				if (!pending->transferAllocator) {
					DX::ThrowIfFailed(owner.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(pending->transferAllocator.put())));
					DX::ThrowIfFailed(owner.d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pending->transferAllocator.get(),
						nullptr, IID_PPV_ARGS(pending->transferCommands.put())));
					pending->transferCommands->SetName(L"NativePBR::ProjectedMipTransfer");
				} else {
					DX::ThrowIfFailed(pending->transferAllocator->Reset());
					DX::ThrowIfFailed(pending->transferCommands->Reset(pending->transferAllocator.get(), nullptr));
				}
				auto reusable = std::move(pending->textures);
				pending->textures.clear();
				pending->textures.reserve(kProjectedTextureCapacity);
				pending->instances.clear();
				pending->instances.reserve(draws.size());
			pending->rasterDraws.clear();
			pending->rasterDraws.reserve(draws.size());
				pending->rasterAliases.clear();
				pending->stamp = stamp; pending->rowCount = 0; pending->missingTextures = 0;
				pending->cacheHits = pending->suppressedTextures = pending->deferredTextures = 0;
				pending->transferredTextures = pending->newTextures = 0;
				pending->readyFence = producer.fence12; pending->readyValue = UINT64_MAX;
				auto* context = owner.d3d11Context.get();
				// Decode each source with locally restored compute state. Commands are
				// ordered before packing and the single publication fence.
				std::vector<PackConstants> records;
				records.reserve(draws.size());
				for (const auto& draw : draws) {
					PackConstants constants{};
					constants.offsets = draw.offsets;
					pending->instances.push_back(draw.instance);
				pending->rasterDraws.push_back(draw.rasterDraw);
					constants.metadata = { draw.instance, draw.descriptor, static_cast<uint32_t>(records.size()), draw.samplerModes };
					for (const auto alias : draw.rasterAliases)
						pending->rasterAliases.emplace_back(alias, constants.metadata[2]);
					for (size_t i = 0; i < draw.views.size(); ++i) {
						submitted = true;
						constants.textures[i] = ImportTexture(*pending, reusable, draw.views[i].get());
						if (draw.views[i] && constants.textures[i] == UINT32_MAX) ++pending->missingTextures;
					}
					records.push_back(constants);
				}
				for (const auto& texture : reusable) pending->allocationBytes -= texture.bytes;
				reusable.clear();
				winrt::com_ptr<ID3D11ComputeShader> previousShader;
				winrt::com_ptr<ID3D11UnorderedAccessView> previousUav;
				winrt::com_ptr<ID3D11Buffer> previousConstants;
				std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> previousReads;
				context->CSGetShader(previousShader.put(), nullptr, nullptr);
				context->CSGetUnorderedAccessViews(0, 1, previousUav.put());
				context->CSGetConstantBuffers(0, 1, previousConstants.put());
				for (uint32_t i = 0; i < previousReads.size(); ++i) context->CSGetShaderResources(i, 1, previousReads[i].put());
				auto* uav = pending->records->uav;
				context->CSSetShader(packShader.get(), nullptr, 0);
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
				for (size_t i = 0; i < draws.size(); ++i) {
					context->UpdateSubresource(packConstants.get(), 0, nullptr, &records[i], 0, 0);
					auto* parameters = packConstants.get();
					context->CSSetConstantBuffers(0, 1, &parameters);
					context->CSSetShaderResources(0, 2, draws[i].constants.data());
					context->Dispatch(1, 1, 1);
				}
				uav = previousUav.get();
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
				auto* oldConstants = previousConstants.get();
				context->CSSetConstantBuffers(0, 1, &oldConstants);
				ID3D11ShaderResourceView* oldReads[]{ previousReads[0].get(), previousReads[1].get() };
				context->CSSetShaderResources(0, 2, oldReads);
				context->CSSetShader(previousShader.get(), nullptr, 0);
				pending->rowCount = static_cast<uint32_t>(draws.size());
				submitted = true;
				const auto value = producer.Next();
				DX::ThrowIfFailed(context->Signal(producer.fence11.get(), value));
				pending->readyValue = value;
				context->Flush();
				DX::ThrowIfFailed(pending->transferCommands->Close());
				if (transferred) {
					DX::ThrowIfFailed(owner.commandQueue->Wait(producer.fence12.get(), value));
					pending->readyFence = transferFence;
					pending->readyValue = UINT64_MAX;
					ID3D12CommandList* lists[]{ pending->transferCommands.get() };
					transferSubmitted = true;
					owner.commandQueue->ExecuteCommandLists(1, lists);
					const auto complete = ++transferValue;
					DX::ThrowIfFailed(owner.commandQueue->Signal(transferFence.get(), complete));
					pending->readyValue = complete;
				}
				current = pending;
				// All subsequent consumers use this same ordered DX12 queue.
				// Cached native resources are immutable; no upload bridge is retained.
				try { CacheSubmitted(*pending); } catch (const std::bad_alloc&) {}
				pending->transferredTextures = transferred;
				pending->newTextures = allocated;
				pending->immutableCacheBytes = cacheBytes;
				failureLogged = false;
			} catch (...) {
				if (!GraphicsBackend::CheckNativeDeviceHealth(backend.d3d12Device.get(), backend.d3d11Device.get())) return;
				if (pending && !transferSubmitted && pending->transferCommands)
					pending->transferCommands->Close();
				if (pending && !pending->transferCommands) pending->transferAllocator = nullptr;
				if (pending && pending->readyValue == UINT64_MAX) {
					pending->allocationBytes = pending->recordBytes;
					for (const auto& texture : pending->textures) pending->allocationBytes += texture.bytes;
					if (transferSubmitted) {
						const auto rescue = ++transferValue;
						if (SUCCEEDED(owner.commandQueue->Signal(transferFence.get(), rescue))) pending->readyValue = rescue;
					} else if (!submitted) pending->readyValue = 0;
					else {
						const auto rescue = producer.Next();
						if (SUCCEEDED(owner.d3d11Context->Signal(producer.fence11.get(), rescue))) {
							pending->readyValue = rescue;
							owner.d3d11Context->Flush();
						}
					}
				}
				if (!failureLogged) {
					logger::warn("[DX12NativeScene] Projected input publication failed");
					failureLogged = true;
				}
			}
		}
	};
}

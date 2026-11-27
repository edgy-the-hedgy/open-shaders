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
#include <span>
#include <stdexcept>
#include <vector>

namespace DX12Native
{
	// The capture budget is shared with the existing bounded draw snapshot pool.
	inline constexpr uint32_t kMaxProjectedDraws = 128;
	inline constexpr uint32_t kProjectedTextureRoles = 4;
	inline constexpr uint32_t kProjectedTextureCapacity = kMaxProjectedDraws * kProjectedTextureRoles;
	inline constexpr uint32_t kProjectedRecordColumns = 8;
	struct ProjectedInputDraw
	{
		uint32_t instance = UINT32_MAX, descriptor = 0;
		std::array<ID3D11ShaderResourceView*, 2> constants{};
		std::array<uint32_t, 4> offsets{};
		std::array<winrt::com_ptr<ID3D11ShaderResourceView>, kProjectedTextureRoles> views;
	};
	struct ProjectedInputFrame
	{
		struct Texture
		{
			winrt::com_ptr<ID3D11ShaderResourceView> source;
			std::unique_ptr<WrappedResource> shared;
			D3D11_SHADER_RESOURCE_VIEW_DESC view{};
			uint64_t bytes = 0;
		};
		MaterialFrameContract::RasterIdentityStamp stamp{};
		std::unique_ptr<WrappedResource> records;
		std::vector<Texture> textures;
		std::vector<uint32_t> instances;
		uint32_t rowCount = 0, missingTextures = 0;
		uint64_t allocationBytes = 0, recordBytes = 0, readyValue = 0;
		winrt::com_ptr<ID3D12Fence> readyFence;
		bool Ready(const MaterialFrameContract::RasterIdentityStamp& requested) const
		{
			return stamp == requested && rowCount && records && records->resource && readyFence &&
				MaterialFrameContract::ValidProducerSignal(readyValue, readyFence->GetCompletedValue());
		}
	};

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
		bool failureLogged = false;

		uint64_t TrackedBytes() const
		{
			uint64_t bytes = 0;
			for (const auto& slot : slots) if (slot) bytes += slot->allocationBytes;
			return bytes;
		}
		uint64_t Allocation(const D3D12_RESOURCE_DESC& desc)
		{
			const auto allocation = owner.d3d12Device->GetResourceAllocationInfo(0, 1, &desc);
			const auto allowance = budget.Query(owner.d3d12Device.get(), TrackedBytes()).effectiveBytes;
			const auto tracked = TrackedBytes();
			if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX ||
				tracked > allowance || allocation.SizeInBytes > allowance - tracked)
				return 0;
			return allocation.SizeInBytes;
		}
		uint32_t ImportTexture(ProjectedInputFrame& frame, std::vector<ProjectedInputFrame::Texture>& reusable, ID3D11ShaderResourceView* view)
		{
			if (!view) return UINT32_MAX;
			for (uint32_t index = 0; index < frame.textures.size(); ++index)
				if (frame.textures[index].source.get() == view) return index;
			if (frame.textures.size() >= kProjectedTextureCapacity) return UINT32_MAX;
			D3D11_SHADER_RESOURCE_VIEW_DESC sourceView{};
			view->GetDesc(&sourceView);
			winrt::com_ptr<ID3D11Resource> source;
			view->GetResource(source.put());
			auto texture = source.try_as<ID3D11Texture2D>();
			D3D11_TEXTURE2D_DESC desc{};
			if (texture) texture->GetDesc(&desc);
			// Preserve the exact typed view and all original mips. Do not reinterpret
			// typeless formats, multisampled/array views, or writable render targets.
			if (!texture || sourceView.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
				sourceView.Format == DXGI_FORMAT_UNKNOWN || sourceView.Format != desc.Format ||
				!desc.Width || !desc.Height || !desc.MipLevels || desc.ArraySize != 1 ||
				desc.SampleDesc.Count != 1 || desc.CPUAccessFlags ||
				(desc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_DEPTH_STENCIL)))
				return UINT32_MAX;
			const auto firstMip = sourceView.Texture2D.MostDetailedMip;
			if (firstMip >= desc.MipLevels) return UINT32_MAX;
			auto& levels = sourceView.Texture2D.MipLevels;
			if (levels == UINT32_MAX) levels = desc.MipLevels - firstMip;
			if (!levels || levels > desc.MipLevels - firstMip) return UINT32_MAX;
			// A retired slot can safely refresh the same shared allocation.
			const auto old = std::find_if(reusable.begin(), reusable.end(), [view](const auto& entry) {
				return entry.source.get() == view && entry.shared;
			});
			if (old != reusable.end()) {
				const auto index = static_cast<uint32_t>(frame.textures.size());
				frame.textures.push_back(std::move(*old));
				old->bytes = 0;
				if (desc.Usage != D3D11_USAGE_IMMUTABLE)
					owner.d3d11Context->CopyResource(frame.textures.back().shared->resource11, texture.get());
				return index;
			}
			D3D12_RESOURCE_DESC allocationDesc{};
			allocationDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			allocationDesc.Width = desc.Width; allocationDesc.Height = desc.Height;
			allocationDesc.DepthOrArraySize = 1; allocationDesc.MipLevels = static_cast<uint16_t>(desc.MipLevels);
			allocationDesc.Format = desc.Format; allocationDesc.SampleDesc = { 1, 0 };
			allocationDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			const auto bytes = Allocation(allocationDesc);
			if (!bytes) return UINT32_MAX;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = desc.MiscFlags = 0;
			ProjectedInputFrame::Texture entry;
			try {
				entry.shared = std::make_unique<WrappedResource>(desc, owner.d3d11Device.get(), owner.d3d12Device.get(), "DX12NativeScene::ProjectedTexture");
			} catch (...) {
				// Some drivers do not support shared resources for particular DDS formats.
				return UINT32_MAX;
			}
			entry.source.copy_from(view);
			entry.view = sourceView; entry.bytes = bytes;
			const auto index = static_cast<uint32_t>(frame.textures.size());
			frame.textures.push_back(std::move(entry));
			frame.allocationBytes += bytes;
			owner.d3d11Context->CopyResource(frame.textures.back().shared->resource11, texture.get());
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
		void Reset() { current.reset(); failureLogged = false; }
		std::shared_ptr<const ProjectedInputFrame> Get(const MaterialFrameContract::RasterIdentityStamp& stamp) const
		{
			return current && current->Ready(stamp) ? current : nullptr;
		}
		void Publish(const GraphicsBackend& backend, MaterialFrameContract::RasterIdentityStamp stamp, std::span<const ProjectedInputDraw> draws)
		{
			current.reset();
			if (draws.empty() || draws.size() > kMaxProjectedDraws || !stamp.scope || !stamp.geometryRevision || !stamp.materialRevision)
				return;
			std::shared_ptr<ProjectedInputFrame> pending;
			bool submitted = false;
			try {
				if (!producer.fence12) {
					owner = backend;
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
				auto reusable = std::move(pending->textures);
				pending->textures.clear();
				pending->textures.reserve(kProjectedTextureCapacity);
				pending->instances.clear();
				pending->instances.reserve(draws.size());
				pending->stamp = stamp; pending->rowCount = 0; pending->missingTextures = 0;
				pending->readyFence = producer.fence12; pending->readyValue = UINT64_MAX;
				auto* context = owner.d3d11Context.get();
				// Import all sources before changing compute state. Copy commands are
				// ordered before packing and the single publication fence.
				std::vector<PackConstants> records;
				records.reserve(draws.size());
				for (const auto& draw : draws) {
					PackConstants constants{};
					constants.offsets = draw.offsets;
					pending->instances.push_back(draw.instance);
					constants.metadata = { draw.instance, draw.descriptor, static_cast<uint32_t>(records.size()), 0 };
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
				current = pending;
				failureLogged = false;
			} catch (...) {
				if (pending && pending->readyValue == UINT64_MAX) {
					pending->allocationBytes = pending->recordBytes;
					for (const auto& texture : pending->textures) pending->allocationBytes += texture.bytes;
					if (!submitted) pending->readyValue = 0;
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

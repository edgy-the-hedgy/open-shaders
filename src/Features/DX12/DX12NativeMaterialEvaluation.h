#pragma once

#include "DX12PreparedMaterials.h"
#include "VideoMemoryBudget.h"
#include <DirectXTex.h>
#include "RE/B/BSResourceNiBinaryStream.h"
#include <directx/d3dx12.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>

namespace DX12Native
{
	// Generation-owned immutable resources. No source DDS is persisted.
	// This evaluates a representative triangle per instance, not screen pixels.
	struct NativeMaterialEvaluation
	{
		struct Texture
		{
			winrt::com_ptr<ID3D12Resource> resource;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			uint32_t mips = 0;
			uint64_t bytes = 0;
			uintmax_t fileBytes = 0;
			bool archiveBacked = false;
			std::filesystem::file_time_type fileStamp{};
		};
		struct Upload
		{
			std::shared_ptr<const Texture> texture;
			winrt::com_ptr<ID3D12Resource> staging;
			std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
		};
		struct Sample
		{
			std::array<float, 4> color, normal, f0, emission;
		};
		static_assert(sizeof(Sample) == 64);
		using TextureCache = std::unordered_map<std::string, std::shared_ptr<const Texture>>;

		TextureCache textures;
		std::vector<std::shared_ptr<const Texture>> poolTextures;
		winrt::com_ptr<ID3D12Resource> records, bindings, output, readback;
		winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
		winrt::com_ptr<ID3D12RootSignature> root;
		winrt::com_ptr<ID3D12PipelineState> pipeline;
		uint64_t revision = 0, textureBytes = 0;
		uint32_t instanceCount = 0, residentRecipes = 0, failedTextures = 0, reusedTextures = 0, archiveTextures = 0;
		uint32_t evaluated = 0;
		std::map<std::string, uint32_t> failureReasons;
		std::vector<std::pair<std::string, std::string>> failedExamples;
		Sample firstSample{};

		static void Check(HRESULT hr)
		{
			if (FAILED(hr)) throw std::runtime_error("NativePBR resource creation failed");
		}

		static winrt::com_ptr<ID3D12Resource> Buffer(ID3D12Device* device, uint64_t bytes,
			D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
		{
			const auto heap = CD3DX12_HEAP_PROPERTIES(type);
			const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
			winrt::com_ptr<ID3D12Resource> result;
			Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(result.put())));
			return result;
		}

		template <class T>
		static winrt::com_ptr<ID3D12Resource> Table(ID3D12Device* device, const std::vector<T>& values)
		{
			auto buffer = Buffer(device, uint64_t(values.size()) * sizeof(T), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
			void* mapped = nullptr;
			const D3D12_RANGE noRead{};
			Check(buffer->Map(0, &noRead, &mapped));
			std::memcpy(mapped, values.data(), values.size() * sizeof(T));
			buffer->Unmap(0, nullptr);
			return buffer;
		}

		static bool SupportedFormat(DXGI_FORMAT format)
		{
			switch (format) {
			case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM:
			case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC7_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8X8_UNORM:
				return true;
			default: return false;
			}
		}

		static std::optional<Upload> LoadTexture(ID3D12Device* device, const std::string& canonical,
			uint64_t& remaining, std::string& reason)
		{
			const auto reject = [&](std::string why) -> std::optional<Upload> { reason = std::move(why); return {}; };
			if (canonical.empty() || !canonical.starts_with("textures/")) return reject("invalid canonical path");
			const auto path = Util::JoinRelativePath("Data", std::filesystem::path(canonical));
			std::error_code ec;
			const bool loose = std::filesystem::is_regular_file(path, ec);
			if (ec && ec != std::errc::no_such_file_or_directory) return reject("file access");
			uint64_t size = 0;
			std::filesystem::file_time_type stamp{};
			std::vector<uint8_t> archiveBytes;
			constexpr uint64_t maxSource = 128ull * 1024 * 1024;
			if (loose) {
				size = std::filesystem::file_size(path, ec);
				if (ec || !size) return reject("empty or unreadable loose file");
				if (size > remaining || size > maxSource) return reject("source budget");
				stamp = std::filesystem::last_write_time(path, ec);
				if (ec) return reject("file timestamp");
			} else {
				// Use the engine's effective archive order; no extraction to disk.
				// Each worker request owns its own stream.
				auto asset = canonical;
				std::ranges::replace(asset, '/', '\\');
				RE::BSResourceNiBinaryStream stream(asset.c_str());
				if (!stream.good() || !stream.stream) return reject("missing loose/archive asset");
				size = stream.stream->totalSize;
				if (!size) return reject("empty archive asset");
				if (size > remaining || size > maxSource) return reject("source budget");
				archiveBytes.resize(size_t(size));
				if (!stream.read(archiveBytes.data(), uint32_t(size))) return reject("archive read");
			}
			// Inspect header before decoding; reject non-2D and oversized input.
			DirectX::TexMetadata meta{};
			const auto metadataResult = loose ?
				DirectX::GetMetadataFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, meta) :
				DirectX::GetMetadataFromDDSMemory(archiveBytes.data(), archiveBytes.size(), DirectX::DDS_FLAGS_NONE, meta);
			if (FAILED(metadataResult)) return reject("invalid DDS metadata");
			if (
				meta.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || meta.arraySize != 1 ||
				meta.IsCubemap() || !meta.width || !meta.height || meta.width > 16384 || meta.height > 16384 ||
				!meta.mipLevels || meta.mipLevels > 15)
				return reject("unsupported DDS dimensions");
			const auto format = DirectX::MakeLinear(meta.format);
			if (!SupportedFormat(format)) return reject("unsupported DXGI format " + std::to_string(unsigned(format)));
			const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DirectX::MakeTypeless(format), meta.width,
				uint32_t(meta.height), 1, uint16_t(meta.mipLevels));
			const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
			Upload upload;
			upload.footprints.resize(meta.mipLevels);
			std::vector<UINT> rows(meta.mipLevels);
			std::vector<UINT64> rowBytes(meta.mipLevels);
			uint64_t stagingBytes = 0;
			device->GetCopyableFootprints(&desc, 0, uint32_t(meta.mipLevels), 0, upload.footprints.data(),
				rows.data(), rowBytes.data(), &stagingBytes);
			if (allocation == UINT64_MAX || !stagingBytes || allocation > remaining || stagingBytes > remaining - allocation)
				return reject("DXGI upload budget");
			DirectX::ScratchImage image;
			const auto decode = loose ? DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NO_LEGACY_EXPANSION, nullptr, image) :
				DirectX::LoadFromDDSMemory(archiveBytes.data(), archiveBytes.size(), DirectX::DDS_FLAGS_NO_LEGACY_EXPANSION, nullptr, image);
			if (FAILED(decode))
				return reject("DDS decode");
			const auto& loaded = image.GetMetadata();
			if (loaded.width != meta.width || loaded.height != meta.height || loaded.format != meta.format ||
				loaded.mipLevels != meta.mipLevels || loaded.arraySize != 1) return reject("DDS changed during read");
			for (uint32_t mip = 0; mip < uint32_t(meta.mipLevels); ++mip) {
				const auto* sub = image.GetImage(mip, 0, 0);
				if (!sub || sub->rowPitch < rowBytes[mip] || sub->slicePitch < sub->rowPitch * rows[mip])
					return reject("invalid DDS row layout");
			}
			auto texture = std::make_shared<Texture>();
			texture->format = format;
			texture->mips = uint32_t(meta.mipLevels);
			texture->bytes = allocation;
			texture->fileBytes = size;
			texture->archiveBacked = !loose;
			texture->fileStamp = stamp;
			const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
			Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr, IID_PPV_ARGS(texture->resource.put())));
			upload.staging = Buffer(device, stagingBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
			void* mapped = nullptr;
			const D3D12_RANGE noRead{};
			Check(upload.staging->Map(0, &noRead, &mapped));
			for (uint32_t mip = 0; mip < texture->mips; ++mip) {
				const auto* sub = image.GetImage(mip, 0, 0);
				const auto& footprint = upload.footprints[mip];
				for (uint32_t row = 0; row < rows[mip]; ++row)
					std::memcpy(static_cast<uint8_t*>(mapped) + size_t(footprint.Offset) + size_t(row) * footprint.Footprint.RowPitch,
						sub->pixels + size_t(row) * sub->rowPitch, size_t(rowBytes[mip]));
			}
			upload.staging->Unmap(0, nullptr);
			upload.texture = std::move(texture);
			remaining -= allocation + stagingBytes;
			return upload;
		}

		void CreatePipeline(ID3D12Device* device)
		{
			std::ifstream file("Data/Shaders/NativeComposite/NativePBRProbe.dxil", std::ios::binary | std::ios::ate);
			if (!file || file.tellg() <= 0 || file.tellg() > 16 * 1024 * 1024)
				throw std::runtime_error("NativePBRProbe.dxil unavailable");
			std::vector<char> shader(static_cast<size_t>(file.tellg()));
			file.seekg(0);
			if (!file.read(shader.data(), std::streamsize(shader.size())))
				throw std::runtime_error("NativePBRProbe.dxil read failed");
			CD3DX12_DESCRIPTOR_RANGE range;
			range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, NativePBR::kGpuTextureDescriptorCapacity, 3);
			CD3DX12_ROOT_PARAMETER parameters[6];
			parameters[0].InitAsShaderResourceView(0);
			parameters[1].InitAsShaderResourceView(1);
			parameters[2].InitAsShaderResourceView(2);
			parameters[3].InitAsDescriptorTable(1, &range);
			parameters[4].InitAsUnorderedAccessView(0);
			parameters[5].InitAsConstants(1, 0);
			std::array<CD3DX12_STATIC_SAMPLER_DESC, 4> samplers;
			for (uint32_t mode = 0; mode < 4; ++mode)
				samplers[mode] = CD3DX12_STATIC_SAMPLER_DESC(mode, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
					(mode & 2) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
					(mode & 1) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
					D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
			const CD3DX12_ROOT_SIGNATURE_DESC desc(6, parameters, UINT(samplers.size()), samplers.data());
			winrt::com_ptr<ID3DBlob> blob, errors;
			Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put()));
			Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(root.put())));
			D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
			pso.pRootSignature = root.get();
			pso.CS = { shader.data(), shader.size() };
			Check(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(pipeline.put())));
		}

		// Called on the existing acceleration worker before recording commands.
		// Reuse only fence-completed textures from a previous publication.
		static std::shared_ptr<NativeMaterialEvaluation> Prepare(ID3D12Device* device,
			const PreparedMaterials& prepared, const SceneInputs& scene, const TextureCache& previous,
			std::vector<Upload>& uploads)
		{
			if (prepared.recipes.empty() || prepared.bindings.empty() ||
				prepared.bindings.size() != scene.geometry.size() || prepared.bindings.size() > 65535u * 64u) return {};
			VideoMemoryBudget budget;
			auto available = budget.Query(device, 0);
			if (!available.dxgiAvailable) return {};
			const uint64_t resultBytes = uint64_t(prepared.bindings.size()) * sizeof(Sample);
			const uint64_t tableBytes = uint64_t(prepared.recipes.size()) * sizeof(NativePBR::GpuMaterialRecord) +
				uint64_t(prepared.bindings.size()) * sizeof(NativePBR::GpuDrawBinding);
			// Reserve aligned allocations for tables, UAV/readback and descriptor metadata.
			const uint64_t overhead = 2 * resultBytes + tableBytes + 6ull * 65536;
			if (overhead >= available.effectiveBytes) return {};
			uint64_t remaining = available.effectiveBytes - overhead;
			auto result = std::make_shared<NativeMaterialEvaluation>();
			result->revision = prepared.sceneRevision;
			result->instanceCount = uint32_t(prepared.bindings.size());
			result->CreatePipeline(device);
			const D3D12_DESCRIPTOR_HEAP_DESC heap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				NativePBR::kGpuTextureDescriptorCapacity, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
			Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(result->descriptors.put())));
			const auto start = result->descriptors->GetCPUDescriptorHandleForHeapStart();
			const auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			const auto write = [&](uint32_t index, const Texture* texture, bool srgb) {
				D3D12_SHADER_RESOURCE_VIEW_DESC view{};
				view.Format = texture ? (srgb ? DirectX::MakeSRGB(texture->format) : texture->format) : DXGI_FORMAT_R8G8B8A8_UNORM;
				view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				view.Texture2DArray.MipLevels = texture ? texture->mips : 1;
				view.Texture2DArray.ArraySize = 1;
				const D3D12_CPU_DESCRIPTOR_HANDLE handle{ start.ptr + SIZE_T(index) * step };
				device->CreateShaderResourceView(texture ? texture->resource.get() : nullptr, &view, handle);
			};
			for (uint32_t i = 0; i < NativePBR::kGpuTextureDescriptorCapacity; ++i) write(i, nullptr, false);
			std::unordered_map<std::string, uint32_t> indices;
			std::unordered_set<std::string> failed;
			std::unordered_map<uint64_t, uint32_t> ready;
			std::vector<NativePBR::GpuMaterialRecord> recordTable;
			recordTable.reserve(prepared.recipes.size());
			auto bindingTable = prepared.bindings;
			for (const auto& recipe : prepared.recipes) {
				NativePBR::GpuTextureLocations locations{};
				for (auto& location : locations) location = { NativePBR::kGpuMissingTexture, 0 };
				for (size_t role = 0; role < 4; ++role) {
					const auto& request = recipe.textures[role];
					const auto& path = request.canonicalPath;
					if (path.empty() || failed.contains(path)) continue;
					auto found = indices.find(path);
					if (found == indices.end()) {
						if (indices.size() >= NativePBR::kGpuTexturePoolCapacity) {
							failed.insert(path); ++result->failedTextures; ++result->failureReasons["descriptor capacity"];
							if (result->failedExamples.size() < 32) result->failedExamples.emplace_back(path, "descriptor capacity");
							continue;
						}
						std::shared_ptr<const Texture> texture;
						if (const auto old = previous.find(path); old != previous.end()) {
							std::error_code ec;
							const auto filename = Util::JoinRelativePath("Data", std::filesystem::path(path));
							const auto size = std::filesystem::file_size(filename, ec);
							if (old->second->archiveBacked && ec == std::errc::no_such_file_or_directory) {
								// Archive load order is immutable during the running game session.
								texture = old->second;
								++result->reusedTextures;
							} else if (!old->second->archiveBacked && !ec && size == old->second->fileBytes) {
								const auto stamp = std::filesystem::last_write_time(filename, ec);
								if (!ec && stamp == old->second->fileStamp) {
									texture = old->second;
									++result->reusedTextures;
								}
							}
						}
						std::string reason;
						if (!texture) {
							// Re-query global headroom before each new allocation.
							const auto current = budget.Query(device, 0);
							remaining = std::min(remaining, current.effectiveBytes);
							try {
								if (auto upload = LoadTexture(device, path, remaining, reason)) {
									texture = upload->texture;
									uploads.push_back(std::move(*upload));
								}
							} catch (const std::exception&) { reason = "GPU allocation/upload setup"; }
						}
						if (!texture) {
							failed.insert(path);
							++result->failedTextures;
							if (reason.empty()) reason = "unknown texture failure";
							++result->failureReasons[reason];
							if (result->failedExamples.size() < 32) result->failedExamples.emplace_back(path, reason);
							continue;
						}
						const uint32_t pool = uint32_t(indices.size());
						write(pool, texture.get(), false);
						write(pool + NativePBR::kGpuTexturePoolCapacity, texture.get(), true);
						result->textureBytes += texture->bytes;
						if (texture->archiveBacked) ++result->archiveTextures;
						result->poolTextures.push_back(texture);
						result->textures.emplace(path, std::move(texture));
						found = indices.emplace(path, pool).first;
					}
					locations[role] = { NativePBR::GpuTextureDescriptorIndex(found->second, request.interpretation), 0 };
				}
				auto record = NativePBR::PackGpuMaterialRecord(recipe, locations);
				const uint32_t index = uint32_t(recordTable.size());
				recordTable.push_back(record);
				const uint32_t required = NativePBR::GpuRequestedTextureRoles(recipe) & 15u;
				if ((record.controls[0] & required) == required &&
					!(recipe.issues & (NativePBR::InvalidSnapshot | NativePBR::InvalidTexturePath | NativePBR::MissingRequiredTextures))) {
					ready.emplace(recipe.identity, index);
					++result->residentRecipes;
				}
			}
			for (size_t i = 0; i < bindingTable.size(); ++i) {
				auto& binding = bindingTable[i];
				const uint64_t identity = uint64_t(binding.identity[0]) | (uint64_t(binding.identity[1]) << 32);
				const auto it = ready.find(identity);
				if (it == ready.end() || !scene.geometry[i].shape || !scene.geometry[i].shape->shading) continue;
				const auto& source = *scene.geometry[i].shape->shading;
				binding.materialIndex = it->second;
				binding.flags = (source.shaderFlags2 & (1u << 5)) ? NativePBR::kGpuDrawUsesVertexColor : 0;
				const auto& normalPath = prepared.recipes[it->second].textures[1].canonicalPath;
				if (const auto normal = result->textures.find(normalPath); normal != result->textures.end() &&
					normal->second->format == DXGI_FORMAT_BC5_UNORM) binding.flags |= 1u << 8;
				if (source.shaderFlags1 & (1u << 12)) binding.flags |= 1u << 9;
				if (source.alphaTest) binding.flags |= (1u << 10) | (uint32_t(source.alphaThreshold) << 16) | (uint32_t(source.alphaFunction) << 24);
			}
			result->records = Table(device, recordTable);
			result->bindings = Table(device, bindingTable);
			result->output = Buffer(device, resultBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			result->readback = Buffer(device, resultBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
			return result;
		}

		void Record(ID3D12GraphicsCommandList* commands, ID3D12Resource* geometry, const std::vector<Upload>& uploads) const
		{
			for (const auto& upload : uploads) {
				for (uint32_t mip = 0; mip < upload.texture->mips; ++mip) {
					const CD3DX12_TEXTURE_COPY_LOCATION destination(upload.texture->resource.get(), mip);
					const CD3DX12_TEXTURE_COPY_LOCATION source(upload.staging.get(), upload.footprints[mip]);
					commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
				}
				const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(upload.texture->resource.get(),
					D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				commands->ResourceBarrier(1, &barrier);
			}
			ID3D12DescriptorHeap* heaps[]{ descriptors.get() };
			commands->SetDescriptorHeaps(1, heaps);
			commands->SetComputeRootSignature(root.get());
			commands->SetPipelineState(pipeline.get());
			commands->SetComputeRootShaderResourceView(0, geometry->GetGPUVirtualAddress());
			commands->SetComputeRootShaderResourceView(1, records->GetGPUVirtualAddress());
			commands->SetComputeRootShaderResourceView(2, bindings->GetGPUVirtualAddress());
			commands->SetComputeRootDescriptorTable(3, descriptors->GetGPUDescriptorHandleForHeapStart());
			commands->SetComputeRootUnorderedAccessView(4, output->GetGPUVirtualAddress());
			commands->SetComputeRoot32BitConstant(5, instanceCount, 0);
			commands->Dispatch((instanceCount + 63u) / 64u, 1, 1);
			const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(output.get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
			commands->ResourceBarrier(1, &barrier);
			commands->CopyBufferRegion(readback.get(), 0, output.get(), 0, uint64_t(instanceCount) * sizeof(Sample));
		}

		// Only the acceleration worker calls this after its producer fence signals.
		void Complete()
		{
			const D3D12_RANGE range{ 0, SIZE_T(instanceCount) * sizeof(Sample) };
			void* mapped = nullptr;
			if (FAILED(readback->Map(0, &range, &mapped))) return;
			const auto* samples = static_cast<const Sample*>(mapped);
			for (uint32_t i = 0; i < instanceCount; ++i) {
				if (samples[i].color[3] > 0.0f && std::isfinite(samples[i].color[3])) {
					if (!evaluated) firstSample = samples[i];
					++evaluated;
				}
			}
			const D3D12_RANGE noWrite{};
			readback->Unmap(0, &noWrite);
			readback = nullptr;
		}
	};
}

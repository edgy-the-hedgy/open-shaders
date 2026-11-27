#pragma once

#include "DX12MaterialFrameContract.h"

#include "DX12PreparedMaterialFrame.h"
#include "DX12NativeSceneInputs.h"
#include "RE/B/BSShaderProperty.h"
#include "VideoMemoryBudget.h"
#include <DirectXTex.h>
#include "RE/B/BSResourceNiBinaryStream.h"
#include <directx/d3dx12.h>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>

namespace DX12Native
{
	// Generation-owned immutable resources. No source DDS is persisted.
	// Texture residency only; screen evaluation belongs to a separate early dispatch.
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
		using TextureCache = std::unordered_map<std::string, std::shared_ptr<const Texture>>;

		TextureCache textures;
		std::vector<std::shared_ptr<const Texture>> poolTextures;
		winrt::com_ptr<ID3D12Resource> records, bindings;
		winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
		uint64_t revision = 0, textureBytes = 0;
		uint32_t instanceCount = 0, residentRecipes = 0, failedTextures = 0, reusedTextures = 0, archiveTextures = 0;
		uint32_t baseOnlyBindings = 0;
		// CPU admission mirror, immutable with the uploaded draw-binding table.
		std::vector<uint8_t> exactBaseCandidates;
		NativePBR::ScreenSurfaceTally surfaceCapabilities;
		uint32_t specializedRequested = 0, specializedResident = 0, sharedTextureReferences = 0;
		uint32_t specializedBlockedByBase = 0;
		std::array<uint32_t, static_cast<size_t>(NativePBR::TextureRole::Count)> specializedRoleRequested{}, specializedRoleResident{};
		std::map<std::string, uint32_t> failureReasons;
		std::vector<std::pair<std::string, std::string>> failedExamples;

		// Rebuild SRVs from immutable texture resources; shader-visible heaps are
		// not legal CopyDescriptors sources. All material consumers share this view ABI.
		static void WriteTextureView(ID3D12Device* device, const Texture* texture, bool srgb, D3D12_CPU_DESCRIPTOR_HANDLE handle)
		{
			if (texture && srgb && DirectX::MakeSRGB(texture->format) == texture->format) texture = nullptr;
			D3D12_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = texture ? (srgb ? DirectX::MakeSRGB(texture->format) : texture->format) : DXGI_FORMAT_R8G8B8A8_UNORM;
			view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
			view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			view.Texture2DArray.MipLevels = texture ? texture->mips : 1;
			view.Texture2DArray.ArraySize = 1;
			device->CreateShaderResourceView(texture ? texture->resource.get() : nullptr, &view, handle);
		}

		void WriteTextureViews(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE start) const
		{
			const auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			for (uint32_t i = 0; i < NativePBR::kGpuTextureDescriptorCapacity; ++i) {
				const auto pool = i % NativePBR::kGpuTexturePoolCapacity;
				const auto* texture = pool < poolTextures.size() ? poolTextures[pool].get() : nullptr;
				WriteTextureView(device, texture, i >= NativePBR::kGpuTexturePoolCapacity, { start.ptr + SIZE_T(i) * step });
			}
		}

		static void Check(HRESULT hr)
		{
			DX::ThrowIfFailed(hr);
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
			case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC7_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8X8_UNORM:
				return true;
			default: return false;
			}
		}

		static std::optional<Upload> LoadTexture(ID3D12Device* device, const std::string& canonical,
			uint64_t& remaining, std::string& reason, bool allowBc4, bool& incompatibleBc4)
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
			if (format == DXGI_FORMAT_BC4_UNORM && !allowBc4) {
				incompatibleBc4 = true;
				return reject("BC4 lacks required material channels");
			}
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
			if (allocation == UINT64_MAX || !stagingBytes || allocation > remaining)
				return reject("DXGI upload budget");
			const auto stagingDesc = CD3DX12_RESOURCE_DESC::Buffer(stagingBytes);
			const auto stagingAllocation = device->GetResourceAllocationInfo(0, 1, &stagingDesc).SizeInBytes;
			if (stagingAllocation == UINT64_MAX || stagingAllocation > remaining - allocation)
				return reject("DXGI upload budget");
			DirectX::ScratchImage image;
			const auto decode = loose ? DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NO_LEGACY_EXPANSION, nullptr, image) :
				DirectX::LoadFromDDSMemory(archiveBytes.data(), archiveBytes.size(), DirectX::DDS_FLAGS_NO_LEGACY_EXPANSION, nullptr, image);
			if (FAILED(decode))
				return reject("DDS decode");
			const auto& loaded = image.GetMetadata();
			if (loaded.width != meta.width || loaded.height != meta.height || loaded.format != meta.format ||
				loaded.mipLevels != meta.mipLevels || loaded.arraySize != 1) return reject("DDS changed during read");
			if (loose) {
				const auto currentSize = std::filesystem::file_size(path, ec);
				if (ec || currentSize != size) return reject("DDS changed during read");
				const auto currentStamp = std::filesystem::last_write_time(path, ec);
				if (ec || currentStamp != stamp) return reject("DDS changed during read");
			}
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
			remaining -= allocation + stagingAllocation;
			return upload;
		}

		static std::shared_ptr<NativeMaterialEvaluation> Prepare(ID3D12Device* device,
			const PreparedMaterialFrame& prepared, const SceneInputs& scene, const TextureCache& previous,
			std::vector<Upload>& uploads)
		{
			if (prepared.recipes.empty() || prepared.bindings.empty() ||
				prepared.bindings.size() != scene.geometry.size() || prepared.bindings.size() > 65535u * 64u) return {};
			VideoMemoryBudget budget;
			auto available = budget.Query(device, 0);
			if (!available.localBudget) return {};
			const uint64_t tableBytes = uint64_t(prepared.recipes.size()) * sizeof(NativePBR::GpuMaterialRecord) +
				uint64_t(prepared.bindings.size()) * sizeof(NativePBR::GpuDrawBinding);
			// Reserve aligned allocations for tables and descriptor metadata.
			const uint64_t overhead = tableBytes + 3ull * 65536;
			if (overhead >= available.effectiveBytes) return {};
			uint64_t remaining = available.effectiveBytes - overhead;
			auto result = std::make_shared<NativeMaterialEvaluation>();
			result->revision = prepared.geometryRevision;
			result->instanceCount = uint32_t(prepared.bindings.size());
			const D3D12_DESCRIPTOR_HEAP_DESC heap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				NativePBR::kGpuTextureDescriptorCapacity, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
			Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(result->descriptors.put())));
			const auto start = result->descriptors->GetCPUDescriptorHandleForHeapStart();
			const auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			const auto write = [&](uint32_t index, const Texture* texture, bool srgb) {
				WriteTextureView(device, texture, srgb, { start.ptr + SIZE_T(index) * step });
			};
			for (uint32_t i = 0; i < NativePBR::kGpuTextureDescriptorCapacity; ++i) write(i, nullptr, false);
			std::unordered_map<std::string, uint32_t> indices;
			std::unordered_set<std::string> failed, invalidSrgb, invalidBc4;
			std::unordered_map<uint64_t, uint32_t> ready;
			std::unordered_set<uint64_t> missingBaseTextures;
			std::vector<NativePBR::GpuMaterialRecord> recordTable;
			recordTable.reserve(prepared.recipes.size());
			auto bindingTable = prepared.bindings;
			std::vector<NativePBR::GpuTextureLocations> recipeLocations(prepared.recipes.size());
			for (auto& locations : recipeLocations)
				for (auto& location : locations) location = { NativePBR::kGpuMissingTexture, 0 };
			for (const bool specialized : { false, true }) {
				for (size_t recipeIndex = 0; recipeIndex < prepared.recipes.size(); ++recipeIndex) {
					const auto& recipe = prepared.recipes[recipeIndex];
					auto& locations = recipeLocations[recipeIndex];
					const auto base = NativePBR::GpuBaseRequestedTextureRoles(recipe);
					if (recipe.issues & (NativePBR::InvalidSnapshot | NativePBR::InvalidTexturePath | NativePBR::MissingRequiredTextures)) continue;
					const auto requested = specialized ? NativePBR::GpuSpecializedTextureRoles(recipe) : base;
					if (specialized) {
						bool baseReady = true;
						for (size_t i = 0; i < locations.size(); ++i)
							if ((base & (1u << i)) && locations[i][0] == NativePBR::kGpuMissingTexture) baseReady = false;
						if (!baseReady) {
							for (size_t i = 0; i < locations.size(); ++i)
								if (requested & (1u << i)) ++result->specializedBlockedByBase;
							continue;
						}
					}
					for (size_t role = 0; role < locations.size(); ++role) {
						if (!(requested & (1u << role))) continue;
						if (specialized) {
							++result->specializedRequested;
							++result->specializedRoleRequested[role];
						}
						const auto& request = recipe.textures[role];
						const auto& path = request.canonicalPath;
						const bool allowBc4 = NativePBR::GpuAcceptsRedOnlyTexture(recipe, static_cast<NativePBR::TextureRole>(role)) &&
							request.interpretation == NativePBR::TextureInterpretation::DataLinear;
						if (path.empty() || failed.contains(path) || (!allowBc4 && invalidBc4.contains(path))) continue;
						const auto rejectBc4 = [&] {
							if (invalidBc4.insert(path).second) {
								const char* reason = request.interpretation == NativePBR::TextureInterpretation::ColorSrgb ?
									"no compatible sRGB view" : "BC4 lacks required material channels";
								++result->failedTextures;
								++result->failureReasons[reason];
								if (result->failedExamples.size() < 32) result->failedExamples.emplace_back(path, reason);
							}
						};
						auto found = indices.find(path);
						if (found == indices.end()) {
							if (indices.size() >= NativePBR::kGpuTexturePoolCapacity) {
								failed.insert(path); ++result->failedTextures; ++result->failureReasons["descriptor capacity"];
								if (result->failedExamples.size() < 32) result->failedExamples.emplace_back(path, "descriptor capacity");
								continue;
							}
							std::shared_ptr<const Texture> texture;
							if (const auto old = previous.find(path); old != previous.end() && old->second && old->second->resource) {
								winrt::com_ptr<ID3D12Device> textureDevice;
								if (SUCCEEDED(old->second->resource->GetDevice(IID_PPV_ARGS(textureDevice.put()))) && textureDevice.get() == device) {
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
							}
							if (texture && texture->format == DXGI_FORMAT_BC4_UNORM && !allowBc4) {
								rejectBc4();
								continue;
							}
							std::string reason;
							bool incompatibleBc4 = false;
							if (!texture) {
								// Re-query global headroom before each new allocation.
								const auto current = budget.Query(device, 0);
								remaining = std::min(remaining, current.effectiveBytes);
								try {
									if (auto upload = LoadTexture(device, path, remaining, reason, allowBc4, incompatibleBc4)) {
										uploads.push_back(std::move(*upload));
										texture = uploads.back().texture;
									}
								} catch (const std::exception&) { reason = "GPU allocation/upload setup"; }
							}
							if (!texture) {
								if (incompatibleBc4) {
									rejectBc4();
									continue;
								}
								failed.insert(path);
								++result->failedTextures;
								if (reason.empty()) reason = "unknown texture failure";
								++result->failureReasons[reason];
								if (result->failedExamples.size() < 32) result->failedExamples.emplace_back(path, reason);
								continue;
							}
							const uint32_t pool = uint32_t(indices.size());
							write(pool, texture.get(), false);
							if (DirectX::MakeSRGB(texture->format) != texture->format)
								write(pool + NativePBR::kGpuTexturePoolCapacity, texture.get(), true);
							result->textureBytes += texture->bytes;
							if (texture->archiveBacked) ++result->archiveTextures;
							result->poolTextures.push_back(texture);
							result->textures.emplace(path, std::move(texture));
							found = indices.emplace(path, pool).first;
						} else {
							++result->sharedTextureReferences;
						}
						const auto format = result->poolTextures[found->second]->format;
						if (format == DXGI_FORMAT_BC4_UNORM && !allowBc4) {
							rejectBc4();
							continue;
						}
						if (request.interpretation == NativePBR::TextureInterpretation::ColorSrgb && DirectX::MakeSRGB(format) == format) {
							if (invalidSrgb.insert(path).second) {
								++result->failedTextures;
								++result->failureReasons["no compatible sRGB view"];
								if (result->failedExamples.size() < 32) result->failedExamples.emplace_back(path, "no compatible sRGB view");
							}
							continue;
						}
						locations[role] = { NativePBR::GpuTextureDescriptorIndex(found->second, request.interpretation), 0 };
						if (specialized) {
							++result->specializedResident;
							++result->specializedRoleResident[role];
						}
					}
				}
			}
			for (size_t recipeIndex = 0; recipeIndex < prepared.recipes.size(); ++recipeIndex) {
				const auto& recipe = prepared.recipes[recipeIndex];
				auto record = NativePBR::PackGpuMaterialRecord(recipe, recipeLocations[recipeIndex]);
				const uint32_t index = uint32_t(recordTable.size());
				recordTable.push_back(record);
				const uint32_t required = NativePBR::GpuBaseRequestedTextureRoles(recipe);
				if ((record.controls[0] & required) == required &&
					!(recipe.issues & (NativePBR::InvalidSnapshot | NativePBR::InvalidTexturePath | NativePBR::MissingRequiredTextures))) {
					ready.emplace(recipe.identity, index);
					++result->residentRecipes;
				} else if (!(recipe.issues & (NativePBR::InvalidSnapshot | NativePBR::InvalidTexturePath))) {
					missingBaseTextures.insert(recipe.identity);
				}
			}
			for (size_t i = 0; i < bindingTable.size(); ++i) {
				auto& binding = bindingTable[i];
				const uint64_t identity = uint64_t(binding.identity[0]) | (uint64_t(binding.identity[1]) << 32);
				const auto it = ready.find(identity);
				if (it == ready.end()) {
					if (missingBaseTextures.contains(identity))
						binding.flags |= MaterialFrameContract::kMissingBaseTexturesBinding;
					continue;
				}
				if (!scene.geometry[i] || !scene.geometry[i]->material) continue;
				const auto& source = *scene.geometry[i]->material;
				if (!source.authoredShaderFlags) continue;
				binding.materialIndex = it->second;
				using F = RE::BSShaderProperty::EShaderPropertyFlag;
				const auto flags = *source.authoredShaderFlags;
				binding.flags = source.colorOffset != UINT32_MAX && (flags & (uint64_t(1) << 37)) ? NativePBR::kGpuDrawUsesVertexColor : 0;
				const auto& recipe = prepared.recipes[it->second];
				const bool authoredProjectedUv = (flags & static_cast<uint64_t>(F::kProjectedUV)) != 0;
				const bool projectedUv = authoredProjectedUv || (source.observedShaderFlags & static_cast<uint64_t>(F::kProjectedUV)) != 0;
				const bool projectionKnown = projectedUv || scene.geometry[i]->materialObjectEditorId.empty();
				auto capability = NativePBR::ClassifyScreenSurface(recipe, projectedUv, true, projectionKnown);
				constexpr uint32_t coat = uint32_t(PBRFlags::TwoLayer);
				constexpr uint32_t interlayer = uint32_t(PBRFlags::InterlayerParallax);
				constexpr uint32_t supportedCoat = coat | uint32_t(PBRFlags::ColoredCoat) | uint32_t(PBRFlags::CoatNormal) | interlayer;
				const auto authoredMap = [&](NativePBR::TextureRole role) {
					return !recipe.textures[static_cast<size_t>(role)].canonicalPath.empty();
				};
				const auto residentMap = [&](NativePBR::TextureRole role) {
					return (recordTable[it->second].controls[0] & NativePBR::TextureRoleBit(role)) != 0;
				};
				// Keep screen displacement feature-limited until runtime parity is verified.
				const bool layeredHeight = (capability.activeFlags & interlayer) &&
					recipe.parameters.kind == NativePBR::MaterialKind::Mesh && recipe.parameters.displacementScale > 0.0f;
				if (!projectedUv && NativePBR::SupportsOpaqueParallax(recipe) && residentMap(NativePBR::TextureRole::Displacement))
					binding.flags |= MaterialFrameContract::kParallaxReadyBinding;
				// Standalone opaque fuzz only. Combined thin, foliage and subsurface
				// variants have separate transmitted-scattering contracts.
				const bool fuzzReady = capability.activeFlags == uint32_t(PBRFlags::Fuzz) &&
					recipe.parameters.kind == NativePBR::MaterialKind::Mesh &&
					(!authoredMap(NativePBR::TextureRole::Features1) || residentMap(NativePBR::TextureRole::Features1)) &&
					(!recipe.parameters.fuzzRoughnessTexture || residentMap(NativePBR::TextureRole::Displacement));
				if (fuzzReady) {
					binding.flags |= MaterialFrameContract::kFuzzReadyBinding;
					capability.issues &= ~(NativePBR::ScreenPbrFeatures | NativePBR::ScreenFeatureTextures);
					// A packed roughness-only map is not active displacement.
					if (recipe.parameters.fuzzRoughnessTexture && recipe.parameters.displacementScale == 0.0f)
						capability.issues &= ~NativePBR::ScreenDisplacement;
				}
				const bool subsurfaceReady = capability.activeFlags == uint32_t(PBRFlags::Subsurface) &&
					(!authoredMap(NativePBR::TextureRole::Features0) || residentMap(NativePBR::TextureRole::Features0));
				if (subsurfaceReady) {
					binding.flags |= MaterialFrameContract::kSubsurfaceReadyBinding;
					capability.issues &= ~(NativePBR::ScreenPbrFeatures | NativePBR::ScreenFeatureTextures);
				}
				constexpr uint32_t thin = uint32_t(PBRFlags::ThinSurface);
				constexpr uint32_t supportedThin = thin | uint32_t(PBRFlags::Subsurface);
				const bool thinReady = (capability.activeFlags & thin) && !(capability.activeFlags & ~supportedThin) &&
					(!authoredMap(NativePBR::TextureRole::Features0) || residentMap(NativePBR::TextureRole::Features0));
				if (thinReady) {
					binding.flags |= MaterialFrameContract::kThinSurfaceReadyBinding;
					capability.issues &= ~(NativePBR::ScreenPbrFeatures | NativePBR::ScreenFeatureTextures);
				}
				const bool coatReady = (capability.activeFlags & coat) && !(capability.activeFlags & ~supportedCoat) &&
					(!authoredMap(NativePBR::TextureRole::Features0) || residentMap(NativePBR::TextureRole::Features0)) &&
					(!authoredMap(NativePBR::TextureRole::Features1) || residentMap(NativePBR::TextureRole::Features1)) &&
					(!(capability.activeFlags & uint32_t(PBRFlags::CoatNormal)) || residentMap(NativePBR::TextureRole::Features1));
				if (coatReady && (!(capability.activeFlags & interlayer) || (!projectedUv && layeredHeight && residentMap(NativePBR::TextureRole::Displacement)))) {
					binding.flags |= MaterialFrameContract::kCoatReadyBinding;
					if (layeredHeight) binding.flags |= MaterialFrameContract::kParallaxReadyBinding;
					// Projection and displacement still retain their own independent limitations.
					capability.issues &= ~(NativePBR::ScreenPbrFeatures | NativePBR::ScreenFeatureTextures);
				}
				result->surfaceCapabilities.Observe(recipe, capability, authoredProjectedUv);
				if (capability.issues) {
					binding.flags |= MaterialFrameContract::kBaseOnlyBinding;
					++result->baseOnlyBindings;
				}
				const auto& normalPath = prepared.recipes[it->second].textures[1].canonicalPath;
				if (const auto normal = result->textures.find(normalPath); normal != result->textures.end() &&
					normal->second->format == DXGI_FORMAT_BC5_UNORM) binding.flags |= 1u << 8;
				if (flags & (uint64_t(1) << 12)) binding.flags |= 1u << 9;
			}
			result->exactBaseCandidates.resize(bindingTable.size());
			for (size_t i = 0; i < bindingTable.size(); ++i) {
				const auto& binding = bindingTable[i];
				result->exactBaseCandidates[i] = binding.materialIndex != UINT32_MAX &&
					!(binding.flags & (MaterialFrameContract::kBaseOnlyBinding | MaterialFrameContract::kMissingBaseTexturesBinding));
			}
			result->records = Table(device, recordTable);
			result->bindings = Table(device, bindingTable);
			return result;
		}

		static void RecordUploads(ID3D12GraphicsCommandList* commands, const std::vector<Upload>& uploads)
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
		}
	};
}

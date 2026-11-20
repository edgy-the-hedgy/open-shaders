#pragma once

#include "Features/DX12NativeScene/MaterialResources.h"
#include "Features/DX12NativeScene/MaterialTextureLookup.h"
#include "Features/DX12NativeScene/VideoMemoryBudget.h"
#include "Features/NativePBR/NativePBRCompiler.h"
#include "Utils/FileDigest.h"
#include <DirectXTex.h>
#include <cctype>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DX12ResourceService
{
	using SnowJson = nlohmann::json;
	/** @brief Scene-independent, versioned inventory of PG-derived snow material bindings and DDS assets. */
	struct GlobalSnowAtlas
	{
		static constexpr uint32_t kSchemaVersion = 2;
		static constexpr size_t kMaximumManifestBytes = 64ull * 1024 * 1024;
		static constexpr size_t kMaximumBatchTextures = 64;
		static constexpr uint32_t kSrgbDescriptorOffset = DX12Native::kMaximumTexturePools;

		struct Texture
		{
			std::string path, sha256, file, status, origin;
			std::vector<std::string> roles;
			uint32_t width = 0, height = 0, mips = 0;
		};

		struct Address
		{
			std::string path, sha256, status;
			uint32_t page = UINT32_MAX, slice = UINT32_MAX;
			uint32_t linearDescriptor = UINT32_MAX, srgbDescriptor = UINT32_MAX;
			bool resident = false;
		};

		/** @brief Owns a shader-visible page table and its corresponding immutable texture generation. */
		struct DescriptorTable
		{
			winrt::com_ptr<ID3D12DescriptorHeap> heap;
			std::shared_ptr<const DX12Native::MaterialResources> residency;
		};

		std::filesystem::path cache;
		std::string configurationFingerprint, contentFingerprint;
		std::unordered_map<std::string, Texture> textures;
		std::unordered_map<std::string, SnowJson> materials;
		uint64_t unresolved = 0, uniqueDdsBlobs = 0;

		/** @brief Validates the immutable global manifest independently of any CELL or TLAS. */
		static std::shared_ptr<const GlobalSnowAtlas> Open(const std::filesystem::path& root,
			bool verifyDdsBytes = true)
		{
			const auto file = root / "manifest.json";
			std::error_code ec;
			if (!std::filesystem::is_regular_file(file, ec) || ec ||
				std::filesystem::file_size(file, ec) > kMaximumManifestBytes || ec)
				throw std::runtime_error("Global snow manifest is missing or oversized");
			std::ifstream input(file, std::ios::binary);
			const auto manifest = SnowJson::parse(input);
			if (!manifest.is_object() || manifest.at("kind") != "dx12-global-snow-atlas" ||
				manifest.at("schemaVersion") != kSchemaVersion ||
				manifest.at("scope") != "global" || manifest.at("cellIndependent") != true)
				throw std::runtime_error("Global snow manifest contract mismatch");
			auto result = std::make_shared<GlobalSnowAtlas>();
			result->cache = root;
			result->configurationFingerprint = manifest.at("configurationFingerprint").get<std::string>();
			result->contentFingerprint = manifest.at("contentFingerprint").get<std::string>();
			result->unresolved = manifest.at("unresolvedResources").get<uint64_t>();
			result->uniqueDdsBlobs = manifest.at("uniqueDdsBlobs").get<uint64_t>();
			if (result->configurationFingerprint.size() != 64 || result->contentFingerprint.size() != 64)
				throw std::runtime_error("Global snow fingerprint length invalid");
			const auto& textures = manifest.at("textures");
			const auto& materials = manifest.at("materials");
			if (!textures.is_array() || textures.size() > 200000 ||
				!materials.is_array() || materials.size() > 200000)
				throw std::runtime_error("Global snow inventory count invalid");
			std::unordered_set<std::string> verifiedBlobs;
			for (const auto& entry : textures) {
				Texture texture;
				texture.path = entry.at("path").get<std::string>();
				texture.status = entry.at("status").get<std::string>();
				texture.origin = entry.at("origin").get<std::string>();
				texture.roles = entry.at("roles").get<std::vector<std::string>>();
				if (texture.path.empty() || texture.path.size() > 512 ||
					!texture.path.starts_with("textures\\") || texture.path.find("..") != std::string::npos)
					throw std::runtime_error("Invalid global snow DDS path");
				if (texture.status == "cached") {
					texture.sha256 = entry.at("sha256").get<std::string>();
					texture.file = entry.at("file").get<std::string>();
					texture.width = entry.at("width").get<uint32_t>();
					texture.height = entry.at("height").get<uint32_t>();
					texture.mips = entry.at("mips").get<uint32_t>();
					if (texture.sha256.size() != 64 || texture.file != "textures/" + texture.sha256 + ".dds" ||
						!texture.width || !texture.height || !texture.mips ||
						!std::filesystem::is_regular_file(root / texture.file, ec) || ec ||
						std::filesystem::file_size(root / texture.file, ec) < 128 || ec ||
						std::filesystem::file_size(root / texture.file, ec) > 128ull * 1024 * 1024 || ec)
						throw std::runtime_error("Global snow DDS cache entry invalid");
					if (verifyDdsBytes && verifiedBlobs.insert(texture.sha256).second) {
						auto digest = Util::FileDigest::Sha256FileHex(root / texture.file);
						if (digest)
							std::ranges::transform(*digest, digest->begin(), [](unsigned char c) { return char(std::tolower(c)); });
						if (!digest || *digest != texture.sha256)
							throw std::runtime_error("Global snow DDS cache digest mismatch");
					}
				}
				if (!result->textures.emplace(texture.path, std::move(texture)).second)
					throw std::runtime_error("Duplicate global snow DDS path");
			}
			for (const auto& entry : materials) {
				const auto id = entry.at("id").get<std::string>();
				if (id.size() != 64 || !result->materials.emplace(id, entry).second)
					throw std::runtime_error("Duplicate or invalid snow material identity");
			}
			return result;
		}

		/** @brief Resolves a global DDS identity to its currently available native page and slice. */
		std::optional<Address> Resolve(const std::string& path,
			const DX12Native::MaterialResources* residency = nullptr) const
		{
			const auto canonical = NativePBR::CanonicalTexturePath(path);
			if (!canonical)
				return std::nullopt;
			std::string key = *canonical;
			std::ranges::replace(key, '/', '\\');
			const auto found = textures.find(key);
			if (found == textures.end())
				return std::nullopt;
			Address address{ found->second.path, found->second.sha256, found->second.status };
			if (residency) {
				const auto location = residency->locations.find(DX12Native::MaterialTextureLookupKey(*canonical));
				if (location != residency->locations.end()) {
					const auto page = residency->logicalLocations.find(location->second);
					if (page != residency->logicalLocations.end()) {
						address.page = page->second[0];
						address.slice = page->second[1];
						address.linearDescriptor = page->second[0];
						address.srgbDescriptor = page->second[0] + kSrgbDescriptorOffset;
						address.resident = !residency->pendingUpload ||
							residency->pendingUpload->Poll() == DX12Native::MaterialUploadStatus::Ready;
					}
				}
			}
			return address;
		}

		/** @brief Stages a requested bounded subset in existing DX12 mip-preserving pages. */
		std::shared_ptr<DX12Native::MaterialResources> LoadPages(const GraphicsBackend& backend,
			const std::vector<std::string>& requestedPaths, uint64_t budgetLimit = 0,
			const DX12Native::MaterialResources* previous = nullptr) const
		{
			if (!backend.d3d12Device || !backend.commandQueue || requestedPaths.size() > kMaximumBatchTextures)
				throw std::runtime_error("Global snow page request is invalid");
			SnowJson entries = SnowJson::array();
			for (const auto& path : requestedPaths) {
				const auto found = Resolve(path);
				if (!found || found->status != "cached")
					continue;
				entries.push_back({ { "path", found->path }, { "status", "cached" },
					{ "sha256", found->sha256 }, { "file", "textures/" + found->sha256 + ".dds" } });
			}
			const auto memory = DX12Native::VideoMemoryBudget{}.Query(backend.d3d12Device.get(),
				previous ? previous->bytes : 0, budgetLimit);
			if (!memory.dxgiAvailable || !memory.effectiveBytes)
				throw std::runtime_error("Global snow GPU residency deferred by DXGI pressure");
			DX12Native::MaterialSource source;
			source.directory = cache;
			source.budget = memory.effectiveBytes;
			source.uploadBatchBytes = 16ull * 1024 * 1024;
			source.textureReferences = std::make_shared<const SnowJson>(std::move(entries));
			return DX12Native::MaterialResources::LoadResidency(backend, source, nullptr, previous);
		}

		/** @brief Creates linear/sRGB SRVs with the same descriptor ABI as NativePBR. */
		static DescriptorTable MakeDescriptors(ID3D12Device* device,
			std::shared_ptr<const DX12Native::MaterialResources> residency)
		{
			if (!device || !residency || residency->pools.size() > DX12Native::kMaximumTexturePools ||
				(residency->pendingUpload && residency->pendingUpload->Poll() != DX12Native::MaterialUploadStatus::Ready))
				throw std::runtime_error("Global snow DX12 pages are not ready for descriptor publication");
			DescriptorTable result;
			result.residency = std::move(residency);
			const D3D12_DESCRIPTOR_HEAP_DESC desc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				2 * DX12Native::kMaximumTexturePools, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
			if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(result.heap.put()))))
				throw std::runtime_error("Global snow descriptor heap creation failed");
			result.heap->SetName(L"DX12Resources::GlobalSnowDescriptors");
			const auto start = result.heap->GetCPUDescriptorHandleForHeapStart();
			const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			for (uint32_t view = 0; view < 2; ++view) {
				for (uint32_t pool = 0; pool < DX12Native::kMaximumTexturePools; ++pool) {
					const auto* page = pool < result.residency->pools.size() ? &result.residency->pools[pool] : nullptr;
					D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
					srv.Format = page ? (view ? DirectX::MakeSRGB(page->format) : page->format) : DXGI_FORMAT_R8G8B8A8_UNORM;
					srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
					srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					srv.Texture2DArray.MipLevels = page ? page->mips : 1;
					srv.Texture2DArray.ArraySize = page ? page->slices : 1;
					const D3D12_CPU_DESCRIPTOR_HANDLE target{ start.ptr + uint64_t(view * DX12Native::kMaximumTexturePools + pool) * increment };
					device->CreateShaderResourceView(page ? page->resource.get() : nullptr, &srv, target);
				}
			}
			return result;
		}
	};
}

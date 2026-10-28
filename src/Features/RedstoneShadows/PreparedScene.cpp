#include "PreparedScene.h"

#include "Utils/FileDigest.h"
#include <BS_thread_pool.hpp>
#include <cctype>
#include <cstring>
#include <fstream>

namespace RedstoneShadow
{
	namespace
	{
		constexpr uint32_t kMaximumInstances = 8192;
		constexpr uint64_t kMaximumManifestBytes = 16ull * 1024 * 1024;

		bool IsDigest(const std::string& a_value)
		{
			return a_value.size() == 64 && std::ranges::all_of(a_value, [](unsigned char c) { return std::isxdigit(c) != 0; });
		}

		std::vector<uint8_t> ReadMesh(const std::filesystem::path& a_path, const std::string& a_digest, uint64_t a_limit)
		{
			const auto file = CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Prepared mesh cannot be opened");
			const SKSE::stl::scope_exit close([&]() noexcept { CloseHandle(file); });
			LARGE_INTEGER size{};
			if (!GetFileSizeEx(file, &size) || size.QuadPart < 16 || uint64_t(size.QuadPart) > a_limit || uint64_t(size.QuadPart) > UINT32_MAX)
				throw std::runtime_error("Prepared mesh exceeds byte budget");
			const auto digest = Util::FileDigest::Sha256HandleHex(file);
			if (!digest || _stricmp(digest->c_str(), a_digest.c_str()) != 0) throw std::runtime_error("Prepared mesh hash mismatch");
			if (!SetFilePointerEx(file, {}, nullptr, FILE_BEGIN)) throw std::runtime_error("Prepared mesh seek failed");
			std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
			DWORD read = 0;
			if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())
				throw std::runtime_error("Prepared mesh read failed");
			return bytes;
		}

		std::shared_ptr<const Geometry> Upload(const GraphicsBackend& a_backend, const std::vector<uint8_t>& a_bytes, uint64_t a_limit)
		{
			std::array<uint32_t, 4> header{};
			std::memcpy(header.data(), a_bytes.data(), sizeof(header));
			const auto [magic, version, vertices, indices] = header;
			const uint64_t vertexBytes = uint64_t(vertices) * 12, indexBytes = uint64_t(indices) * 4;
			if (magic != 0x4d475352 || version != 1 || !vertices || !indices || indices % 3 || 16 + vertexBytes + indexBytes != a_bytes.size())
				throw std::runtime_error("Invalid prepared RSGM header");
			for (uint64_t offset = 16; offset < 16 + vertexBytes; offset += 4) {
				float value;
				std::memcpy(&value, a_bytes.data() + offset, 4);
				if (!std::isfinite(value)) throw std::runtime_error("Non-finite prepared vertex");
			}
			for (uint64_t offset = 16 + vertexBytes; offset < a_bytes.size(); offset += 4) {
				uint32_t index;
				std::memcpy(&index, a_bytes.data() + offset, 4);
				if (index >= vertices) throw std::runtime_error("Prepared index exceeds vertex range");
			}
			auto mesh = std::make_shared<Mesh>();
			mesh->stride = 12;
			mesh->vertexCount = vertices;
			auto geometry = std::make_shared<Geometry>();
			geometry->prepared = true;
			geometry->source = std::move(mesh);
			geometry->indexOffset = vertexBytes;
			geometry->validationOffset = vertexBytes + indexBytes;
			const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
			const auto desc = CD3DX12_RESOURCE_DESC::Buffer(geometry->validationOffset + 4);
			const auto allocation = a_backend.d3d12Device->GetResourceAllocationInfo(0, 1, &desc);
			if (allocation.SizeInBytes == UINT64_MAX || allocation.SizeInBytes > a_limit) throw std::runtime_error("Prepared upload budget exceeded");
			geometry->allocationBytes = allocation.SizeInBytes;
			DX::ThrowIfFailed(a_backend.d3d12Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(geometry->buffer.put())));
			geometry->buffer->SetName(L"RedstoneShadows::PreparedGeometry");
			void* mapped = nullptr;
			const D3D12_RANGE noRead{ 0, 0 };
			DX::ThrowIfFailed(geometry->buffer->Map(0, &noRead, &mapped));
			std::memcpy(mapped, a_bytes.data() + 16, a_bytes.size() - 16);
			std::memset(static_cast<uint8_t*>(mapped) + geometry->validationOffset, 0, 4);
			geometry->buffer->Unmap(0, nullptr);
			return geometry;
		}

		std::shared_ptr<const PreparedCell> Load(const GraphicsBackend& a_backend, const std::filesystem::path& a_directory,
			const std::string& a_cell, uint64_t a_epoch, uint64_t a_budget)
		{
			const auto manifestPath = a_directory / "cell-layout.json";
			if (std::filesystem::file_size(manifestPath) > kMaximumManifestBytes) throw std::runtime_error("Prepared manifest exceeds limit");
			std::ifstream file(manifestPath);
			const auto manifest = json::parse(file);
			if (manifest.at("schemaVersion").get<uint32_t>() != 1 || manifest.at("rendererSchemaVersion").get<uint32_t>() != 1 || _stricmp(manifest.at("cell").get_ref<const std::string&>().c_str(), a_cell.c_str()) != 0)
				throw std::runtime_error("Prepared cell identity/schema mismatch");
			const auto& rows = manifest.at("instances");
			if (!rows.is_array() || rows.empty() || rows.size() > kMaximumInstances) throw std::runtime_error("Prepared instance count invalid");
			auto result = std::make_shared<PreparedCell>();
			result->digest = manifest.at("layoutDigest").get<std::string>();
			if (!IsDigest(result->digest)) throw std::runtime_error("Prepared layout digest invalid");
			auto snapshot = std::make_shared<Snapshot>();
			snapshot->generation = snapshot->sweep = a_epoch;
			snapshot->scope.scene = a_epoch;
			snapshot->scope.batch = a_epoch;
			snapshot->truncated = true;
			snapshot->offline = true;
			std::unordered_map<std::string, std::shared_ptr<const Geometry>> geometries;
			for (const auto& row : rows) {
				const auto hash = row.at("geometrySha256").get<std::string>();
				if (!IsDigest(hash) || row.at("geometryFile").get<std::string>() != "geometry/" + hash + ".rsgm")
					throw std::runtime_error("Prepared geometry path invalid");
				auto found = geometries.find(hash);
				if (found == geometries.end()) {
					if (result->bytes >= a_budget) throw std::runtime_error("Prepared package budget exhausted");
					const auto bytes = ReadMesh(a_directory / "geometry" / (hash + ".rsgm"), hash, a_budget - result->bytes);
					auto geometry = Upload(a_backend, bytes, a_budget - result->bytes);
					result->bytes += geometry->allocationBytes;
					result->geometries.emplace(geometry->source.get(), geometry);
					found = geometries.emplace(hash, std::move(geometry)).first;
				}
				Instance instance;
				instance.identity = snapshot->instances.size() + 1;
				instance.mesh = found->second->source;
				instance.indexCount = row.at("indexCount").get<uint32_t>();
				if (uint64_t(instance.indexCount) * 4 != found->second->validationOffset - found->second->indexOffset)
					throw std::runtime_error("Prepared instance range mismatch");
				const auto matrix = row.at("shapeToCell").get<std::vector<float>>();
				if (matrix.size() != 12 || !std::ranges::all_of(matrix, [](float v) { return std::isfinite(v); }))
					throw std::runtime_error("Prepared instance transform invalid");
				const double determinant = double(matrix[0]) * (double(matrix[5]) * matrix[10] - double(matrix[6]) * matrix[9]) -
					double(matrix[1]) * (double(matrix[4]) * matrix[10] - double(matrix[6]) * matrix[8]) +
					double(matrix[2]) * (double(matrix[4]) * matrix[9] - double(matrix[5]) * matrix[8]);
				if (!std::isfinite(determinant) || determinant == 0) throw std::runtime_error("Prepared instance transform is singular");
				std::memcpy(instance.world.m, matrix.data(), sizeof(instance.world));
				const auto bounds = row.at("cellBounds").get<std::vector<float>>();
				if (bounds.size() != 6 || !std::ranges::all_of(bounds, [](float v) { return std::isfinite(v); }))
					throw std::runtime_error("Prepared instance bounds invalid");
				for (size_t axis = 0; axis < 3; ++axis)
					if (bounds[axis] > bounds[axis + 3]) throw std::runtime_error("Prepared instance bounds reversed");
				instance.bound = { (bounds[0] + bounds[3]) * 0.5f, (bounds[1] + bounds[4]) * 0.5f, (bounds[2] + bounds[5]) * 0.5f,
					std::max({ bounds[3] - bounds[0], bounds[4] - bounds[1], bounds[5] - bounds[2] }) * 0.5f };
				instance.material.opaqueCandidate = row.at("opaqueOccluderCandidate").get<bool>();
				const auto status = row.at("geometryStatus").get<std::string>();
				if (status != "compiled") {
					instance.material.opaqueCandidate = false;
					if (status == "preview-only:alpha-test-or-blend") instance.material.offlineMaterialFlags = OfflineMaterialFlags::AlphaCoverage;
					else if (status == "preview-only:vertex-or-palette-alpha") instance.material.offlineMaterialFlags = OfflineMaterialFlags::VertexAlpha;
					else if (status == "preview-only:controlled-material") instance.material.offlineMaterialFlags = OfflineMaterialFlags::AnimatedMaterial;
					else instance.material.offlineMaterialFlags = OfflineMaterialFlags::Unknown;
				}
				instance.material.twoSided = row.at("twoSided").get<bool>();
				if (!instance.material.opaqueCandidate) ++result->maskedInstances;
				snapshot->instances.push_back(std::move(instance));
			}
			for (const auto& light : manifest.at("lights")) {
				if (result->lights.size() >= 256) throw std::runtime_error("Prepared light count exceeds limit");
				if (light.at("classification").get<std::string>() != "placed-light-baseline") continue;
				const auto position = light.at("position").get<std::vector<float>>();
				if (position.size() != 3 || !std::ranges::all_of(position, [](float v) { return std::isfinite(v); })) continue;
				result->lights.push_back({ position[0], position[1], position[2] });
			}
			result->snapshot = std::move(snapshot);
			return result;
		}
	}

	struct PreparedScene::State
	{
		std::atomic<bool> busy{ false };
		uint64_t completedRevision = 0;
		std::shared_ptr<const PreparedCell> cell;
		std::string failure;
	};

	void PreparedScene::Reset()
	{
		++revision;
		requested = false;
	}

	void PreparedScene::Request(const GraphicsBackend& a_backend, std::filesystem::path a_directory, std::string a_cell, uint64_t a_epoch, uint64_t a_budget)
	{
		if (!state) state = std::make_shared<State>();
		if (requested || state->busy.exchange(true, std::memory_order_acq_rel)) return;
		try {
			static BS::thread_pool<> pool{ 1 };
			pool.detach_task([owned = state, backend = a_backend, directory = std::move(a_directory), cell = std::move(a_cell),
				epoch = a_epoch, budget = a_budget, request = revision] {
				std::shared_ptr<const PreparedCell> loaded;
				std::string failure;
				try { loaded = Load(backend, directory, cell, epoch, budget); }
				catch (const std::exception& error) { failure = error.what(); }
				owned->cell = std::move(loaded);
				owned->failure = std::move(failure);
				owned->completedRevision = request;
				owned->busy.store(false, std::memory_order_release);
			});
			requested = true;
		} catch (const std::exception& error) {
			state->failure = error.what();
			state->completedRevision = revision;
			state->busy.store(false, std::memory_order_release);
			requested = true;
		}
	}

	std::shared_ptr<const PreparedCell> PreparedScene::GetCell() const
	{
		return state && !state->busy.load(std::memory_order_acquire) && state->completedRevision == revision ? state->cell : nullptr;
	}

	json PreparedScene::GetDiagnostics() const
	{
		const bool busy = state && state->busy.load(std::memory_order_acquire);
		const auto cell = GetCell();
		return { { "loading", busy }, { "loaded", bool(cell) }, { "requested", requested },
			{ "lightCount", cell ? cell->lights.size() : 0 },
			{ "failure", state && !busy && state->completedRevision == revision ? state->failure : "" },
			{ "geometryBytes", cell ? cell->bytes : 0 }, { "meshes", cell ? cell->geometries.size() : 0 },
			{ "instances", cell ? cell->snapshot->instances.size() : 0 }, { "maskedInstances", cell ? cell->maskedInstances : 0 },
			{ "layoutDigest", cell ? cell->digest : "" }, { "runtimeCoverageValid", false }, { "pluginStateValidated", false } };
	}
}

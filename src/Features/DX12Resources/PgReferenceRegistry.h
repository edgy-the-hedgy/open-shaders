#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DX12ResourceService
{
    /** @brief Cached, PG-only metadata index; never performs asset discovery. */
    struct PgReferenceRegistry
    {
        static constexpr uint32_t kSchemaVersion = 2;
        static constexpr uint64_t kMaximumIndexBytes = 128ull * 1024 * 1024;

        struct Material
        {
            std::string id, classification, shaderType, projection;
            uint32_t shaderFlags1 = 0, shaderFlags2 = 0;
            float alpha = 1.0f;
            std::array<int32_t, 9> textureSlots{};
        };

        struct Shape
        {
            std::string name, snowCoverage;
            uint32_t materialIndex = 0;
        };

        struct Mesh
        {
            std::string sha256, sourcePackage;
            std::unordered_map<int32_t, Shape> shapes;
        };

        struct Lookup
        {
            const Mesh* mesh;
            const Shape* shape;
            const Material* material;
        };

        std::string generatedUtc, pgDiffSha256, deploymentSha256, pluginsSha256;
        std::vector<std::string> textures;
        std::vector<Material> materials;
        std::unordered_map<std::string, Mesh> meshes;
        uint64_t shapeCount = 0, unresolved = 0;

        static std::shared_ptr<const PgReferenceRegistry> Open(const std::filesystem::path& root)
        {
            const auto path = root / "index-v2.json";
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error) || error ||
                std::filesystem::file_size(path, error) > kMaximumIndexBytes || error)
                throw std::runtime_error("PG reference index missing or oversized");
            std::ifstream input(path, std::ios::binary);
            const auto doc = nlohmann::json::parse(input);
            if (!doc.is_object() || doc.at("kind") != "dx12-pg-references" ||
                doc.at("schemaVersion") != kSchemaVersion ||
                doc.at("discoveryScope") != "pg-patcher-outputs-only" ||
                doc.at("generatedBy") != "explicit-confirmed-user-action")
                throw std::runtime_error("PG reference index contract mismatch");

            auto result = std::make_shared<PgReferenceRegistry>();
            result->generatedUtc = doc.at("generatedUtc").get<std::string>();
            result->pgDiffSha256 = doc.at("pgDiffSha256").get<std::string>();
            result->deploymentSha256 = doc.at("deploymentSha256").get<std::string>();
            result->pluginsSha256 = doc.at("pluginsSha256").get<std::string>();
            for (const auto& hash : { result->pgDiffSha256, result->deploymentSha256, result->pluginsSha256 })
                if (hash.size() != 64 || !std::ranges::all_of(hash, [](unsigned char c) { return std::isxdigit(c) != 0; }))
                    throw std::runtime_error("Invalid PG source digest");
            result->unresolved = doc.at("unresolved").get<uint64_t>();

            const auto& textureRows = doc.at("textures");
            const auto& materialRows = doc.at("materials");
            const auto& meshRows = doc.at("meshes");
            if (!textureRows.is_array() || textureRows.size() > 250000 ||
                !materialRows.is_array() || materialRows.size() > 250000 ||
                !meshRows.is_array() || meshRows.size() > 150000)
                throw std::runtime_error("PG reference index exceeds bounded counts");
            std::unordered_set<std::string> uniqueTextures;
            for (const auto& row : textureRows) {
                auto texture = row.get<std::string>();
                if (texture.size() > 512 || !texture.starts_with("textures/") ||
                    texture.find("..") != std::string::npos || !texture.ends_with(".dds") ||
                    !uniqueTextures.insert(texture).second)
                    throw std::runtime_error("Invalid or duplicated PG texture path");
                result->textures.push_back(std::move(texture));
            }
            std::unordered_set<std::string> uniqueMaterials;
            for (const auto& row : materialRows) {
                Material material;
                material.id = row.at("id").get<std::string>();
                material.classification = row.at("classification").get<std::string>();
                material.shaderType = row.at("shaderType").get<std::string>();
                material.projection = row.at("projection").get<std::string>();
                material.shaderFlags1 = row.at("shaderFlags1").get<uint32_t>();
                material.shaderFlags2 = row.at("shaderFlags2").get<uint32_t>();
                material.alpha = row.at("alpha").get<float>();
                material.textureSlots = row.at("textureSlots").get<std::array<int32_t, 9>>();
                if (material.id.size() != 64 || !uniqueMaterials.insert(material.id).second ||
                    (material.classification != "ordinary" && material.classification != "snow" &&
                        material.classification != "ice" && material.classification != "ambiguous"))
                    throw std::runtime_error("Invalid or duplicated PG material identity");
                for (const auto slot : material.textureSlots)
                    if (slot < -1 || (slot >= 0 && size_t(slot) >= result->textures.size()))
                        throw std::runtime_error("PG material texture index out of bounds");
                result->materials.push_back(std::move(material));
            }
            for (const auto& row : meshRows) {
                const auto path = row.at("path").get<std::string>();
                if (path.size() > 512 || !path.starts_with("meshes\\") ||
                    path.find("..") != std::string::npos || !path.ends_with(".nif"))
                    throw std::runtime_error("Invalid PG mesh path");
                Mesh mesh;
                mesh.sha256 = row.at("sha256").get<std::string>();
                if (row.contains("sourcePackage") && !row.at("sourcePackage").is_null())
                    mesh.sourcePackage = row.at("sourcePackage").get<std::string>();
                if (mesh.sha256.size() != 64)
                    throw std::runtime_error("Invalid PG mesh digest");
                const auto& shapes = row.at("shapes");
                if (!shapes.is_array() || shapes.size() > 100000)
                    throw std::runtime_error("PG mesh shape count exceeded");
                for (const auto& source : shapes) {
                    const auto index = source.at("blockIndex").get<int32_t>();
                    const auto materialIndex = source.at("materialIndex").get<uint32_t>();
                    auto coverage = source.at("snowCoverage").get<std::string>();
                    if (index < 0 || materialIndex >= result->materials.size() ||
                        (coverage != "none" && coverage != "projected-eligible" &&
                            coverage != "native-snow-material"))
                        throw std::runtime_error("Invalid PG shape binding");
                    Shape shape{ source.at("name").get<std::string>(), std::move(coverage), materialIndex };
                    if (!mesh.shapes.emplace(index, std::move(shape)).second)
                        throw std::runtime_error("Duplicate PG mesh block index");
                    ++result->shapeCount;
                }
                if (!result->meshes.emplace(path, std::move(mesh)).second)
                    throw std::runtime_error("Duplicate PG mesh identity");
            }
            if (result->meshes.size() != doc.at("meshCount").get<uint64_t>() ||
                result->shapeCount != doc.at("shapeCount").get<uint64_t>() ||
                result->materials.size() != doc.at("materialCount").get<uint64_t>() ||
                result->textures.size() != doc.at("textureCount").get<uint64_t>())
                throw std::runtime_error("PG reference index count mismatch");
            return result;
        }

        /** @brief Resolves only indexed PG meshes; never searches the current Data directory. */
        std::optional<Lookup> Find(std::string_view meshPath, int32_t blockIndex) const
        {
            std::string key(meshPath);
            std::ranges::transform(key, key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            std::ranges::replace(key, '/', '\\');
            if (!key.starts_with("meshes\\")) key.insert(0, "meshes\\");
            const auto mesh = meshes.find(key);
            if (mesh == meshes.end()) return std::nullopt;
            const auto shape = mesh->second.shapes.find(blockIndex);
            if (shape == mesh->second.shapes.end()) return std::nullopt;
            return Lookup{ &mesh->second, &shape->second, &materials[shape->second.materialIndex] };
        }
    };
}

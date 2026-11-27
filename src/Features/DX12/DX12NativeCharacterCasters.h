#pragma once

#include "DX12GpuRetirement.h"
#include "GraphicsBackend.h"
#include "Globals.h"
#include "RE/B/BSLightingShaderProperty.h"
#include "DX12NativeMaterialEvaluation.h"
#include "DX12CharacterMaterialWorker.h"
#include "VideoMemoryBudget.h"
#include "Utils/ActorUtils.h"
#include "Utils/FileSystem.h"
#include "Utils/SkinnedMesh.h"

#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <map>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace DX12Native
{
    // Frame-local world-space actor geometry. The posed vertex and primitive
    // part streams retire with the exact BLAS generation. Never use static scene IDs.
    struct CharacterCasterFrame
    {
        uint32_t frame = UINT32_MAX, scope = 0, parts = 0, vertices = 0, triangles = 0;
        uint64_t allocationBytes = 0, readyValue = 0;
        uint32_t allowedAge = 0, rigidInstances = 0;
        static constexpr uint32_t kPosedVertexStride = 72;
        // The index resource holds triangles followed by one part ordinal per primitive.
        uint32_t PrimitivePartOffset() const { return triangles * 3u * sizeof(uint32_t); }
        std::shared_ptr<const CharacterCasterFrame> actorSource;
        std::shared_ptr<const CharacterMaterialPublication> materials;
        std::vector<winrt::com_ptr<ID3D12Resource>> rigidResources;
        winrt::com_ptr<ID3D12Fence> readyFence;
        winrt::com_ptr<ID3D12Resource> acceleration, bottom, positions, scratch, bind, palette, indices, instance;
        winrt::com_ptr<ID3D12CommandAllocator> allocator;
        winrt::com_ptr<ID3D12GraphicsCommandList4> commands;
        winrt::com_ptr<ID3D12RootSignature> root;
        winrt::com_ptr<ID3D12PipelineState> pipeline;

        bool Submitted(uint32_t expectedFrame, uint32_t expectedScope) const
        {
            return expectedFrame - frame <= allowedAge && scope == expectedScope && acceleration && readyFence &&
                readyValue != 0 && readyValue != UINT64_MAX && readyFence->GetCompletedValue() != UINT64_MAX;
        }
    };

    class NativeCharacterCasters
    {
        using Eval = NativeMaterialEvaluation;
        static constexpr uint32_t kMaxActors = 24, kMaxParts = 128, kMaxVertices = 262144,
            kMaxIndices = 786432, kMaxBones = 8192, kMaxPartitions = 64;
        struct Vertex
        {
            std::array<float, 3> position;
            std::array<uint32_t, 4> bones;
            std::array<float, 4> weights;
            std::array<float, 3> normal{};
            std::array<float, 2> uv{};
            std::array<float, 4> color{ 1, 1, 1, 1 };
            std::array<float, 3> tangent{}, bitangent{};
        };
        using Bone = std::array<float, 12>;
        static_assert(sizeof(Vertex) == 104 && sizeof(Bone) == 48);
        static_assert(offsetof(Vertex, normal) == 44 && offsetof(Vertex, uv) == 56 &&
            offsetof(Vertex, color) == 64 && offsetof(Vertex, tangent) == 80 && offsetof(Vertex, bitangent) == 92);

        struct Snapshot
        {
            std::vector<Vertex> vertices;
            std::vector<Bone> bones;
            std::vector<uint32_t> indices, primitiveParts;
            std::vector<DX12::PreparedMaterialSource> materialSources;
            uint32_t parts = 0, rejected = 0;
        };

        static Bone Rows(const RE::NiTransform& transform)
        {
            Bone result{};
            for (uint32_t row = 0; row < 3; ++row)
                for (uint32_t col = 0; col < 3; ++col)
                    result[row * 4 + col] = transform.rotate.entry[row][col] * transform.scale;
            result[3] = transform.translate.x; result[7] = transform.translate.y; result[11] = transform.translate.z;
            return result;
        }

        template <class T>
        static void UpdateTable(ID3D12Resource* resource, const std::vector<T>& values)
        {
            void* mapped = nullptr;
            const D3D12_RANGE noRead{};
            Eval::Check(resource->Map(0, &noRead, &mapped));
            std::memcpy(mapped, values.data(), values.size() * sizeof(T));
            resource->Unmap(0, nullptr);
        }

        static bool Finite(const auto& values)
        {
            return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
        }

        static bool InsideBound(const RE::NiBound& bound, const std::array<float, 3>& p)
        {
            if (!std::isfinite(bound.radius) || bound.radius <= 0 || !Finite(p) ||
                !Finite(std::array{ bound.center.x, bound.center.y, bound.center.z })) return false;
            const float tolerance = bound.radius * 1.25f + 2.0f;
            const float x = p[0] - bound.center.x, y = p[1] - bound.center.y, z = p[2] - bound.center.z;
            return std::isfinite(tolerance) && x*x + y*y + z*z <= tolerance*tolerance;
        }

        // Match the existing native static vertex decoder. A missing or invalid
        // shading layout does not disqualify an otherwise valid shadow caster.
        static bool HasColor(RE::BSGraphics::VertexDesc desc)
        {
            using V = RE::BSGraphics::Vertex;
            if (!desc.HasFlag(V::VF_COLORS) || desc.GetSize() < 20) return false;
            const auto offset = desc.GetAttributeOffset(V::VA_COLOR);
            return offset >= 16 && offset <= desc.GetSize() - 4;
        }

        static void CaptureAttributes(RE::BSGraphics::VertexDesc desc, const uint8_t* data, Vertex& vertex)
        {
            using V = RE::BSGraphics::Vertex;
            const auto stride = desc.GetSize();
            const auto valid = [stride](uint32_t offset) {
                return stride >= 20 && offset >= 16 && offset <= stride - 4;
            };
            if (!desc.HasFlag(V::VF_UV) || !desc.HasFlag(V::VF_NORMAL)) return;
            const auto uvOffset = desc.GetAttributeOffset(V::VA_TEXCOORD0);
            const auto normalOffset = desc.GetAttributeOffset(V::VA_NORMAL);
            if (!valid(uvOffset) || !valid(normalOffset)) return;
            uint16_t packedUV[2];
            std::memcpy(packedUV, data + uvOffset, sizeof(packedUV));
            const std::array uv{ DirectX::PackedVector::XMConvertHalfToFloat(packedUV[0]),
                DirectX::PackedVector::XMConvertHalfToFloat(packedUV[1]) };
            if (!Finite(uv)) return;
            const auto sn = [](uint8_t value) { return float(value) * (2.0f / 255.0f) - 1.0f; };
            const auto* n = data + normalOffset;
            vertex.uv = uv;
            vertex.normal = { sn(n[0]), sn(n[1]), sn(n[2]) };
            if (desc.HasFlag(V::VF_TANGENT)) {
                const auto offset = desc.GetAttributeOffset(V::VA_BINORMAL);
                float bx;
                std::memcpy(&bx, data + 12, sizeof(bx));
                if (valid(offset) && std::isfinite(bx)) {
                    const auto* t = data + offset;
                    vertex.tangent = { sn(t[0]), sn(t[1]), sn(t[2]) };
                    vertex.bitangent = { bx, sn(n[3]), sn(t[3]) };
                }
            }
            if (desc.HasFlag(V::VF_COLORS)) {
                const auto offset = desc.GetAttributeOffset(V::VA_COLOR);
                if (valid(offset))
                    for (uint32_t i = 0; i < 4; ++i) vertex.color[i] = float(data[offset + i]) / 255.0f;
            }
        }

        static bool CaptureRigid(RE::BSGeometry& geometry, Snapshot& out)
        {
            auto* tri = geometry.AsTriShape();
            const auto& runtime = geometry.GetGeometryRuntimeData();
            if (!tri || !runtime.rendererData || !runtime.rendererData->rawVertexData ||
                !runtime.rendererData->rawIndexData || out.bones.size() >= kMaxBones)
                return false;
            auto desc = runtime.vertexDesc;
            const auto stride = desc.GetSize();
            const auto counts = tri->GetTrishapeRuntimeData();
            const size_t count = counts.vertexCount, indexCount = size_t(counts.triangleCount) * 3;
            if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || stride < 16 || stride > 256 ||
                !count || !indexCount || count > 65535 || out.vertices.size() + count > kMaxVertices ||
                out.indices.size() + indexCount > kMaxIndices) return false;
            const auto transform = Rows(geometry.world);
            if (!Finite(transform)) return false;
            std::vector<Vertex> vertices;
            vertices.reserve(count);
            const uint32_t bone = uint32_t(out.bones.size()), base = uint32_t(out.vertices.size());
            for (size_t i = 0; i < count; ++i) {
                Vertex v{ {}, { bone, bone, bone, bone }, { 1, 0, 0, 0 } };
                std::memcpy(v.position.data(), runtime.rendererData->rawVertexData + i * stride, sizeof(v.position));
                if (!Finite(v.position)) return false;
                CaptureAttributes(desc, runtime.rendererData->rawVertexData + i * stride, v);
                vertices.push_back(v);
            }
            const size_t step = std::max<size_t>(1, count / 32);
            for (size_t i = 0; i < count; i += step) {
                const auto& v = vertices[i].position;
                std::array<float, 3> p{};
                for (uint32_t row = 0; row < 3; ++row)
                    p[row] = transform[row*4]*v[0] + transform[row*4+1]*v[1] + transform[row*4+2]*v[2] + transform[row*4+3];
                if (!InsideBound(geometry.worldBound, p)) return false;
            }
            std::vector<uint32_t> indices;
            indices.reserve(indexCount);
            for (size_t i = 0; i < indexCount; ++i) {
                const auto index = runtime.rendererData->rawIndexData[i];
                if (index >= count) return false;
                indices.push_back(base + index);
            }
            out.vertices.insert(out.vertices.end(), vertices.begin(), vertices.end());
            out.bones.push_back(transform);
            out.indices.insert(out.indices.end(), indices.begin(), indices.end());
            out.primitiveParts.insert(out.primitiveParts.end(), indices.size() / 3, out.parts);
            out.materialSources.push_back(CaptureCharacterMaterial(geometry, HasColor(desc)));
            ++out.parts;
            return true;
        }

        static bool CapturePart(RE::BSGeometry* geometry, Snapshot& out)
        {
            if (!geometry || geometry->GetAppCulled() || geometry->AsDynamicTriShape())
                return false;
            const auto& runtime = geometry->GetGeometryRuntimeData();
            const auto* property = runtime.shaderProperty.get();
            if (runtime.alphaProperty || !property ||
                property->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get()) return false;
            using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
            if (!property->flags.any(Flag::kCastShadows) || property->flags.any(Flag::kVertexAlpha) ||
                property->flags.any(Flag::kScreendoorAlphaFade) || property->flags.any(Flag::kRefraction) ||
                property->flags.any(Flag::kTreeAnim) || property->flags.any(Flag::kDecal) ||
                property->flags.any(Flag::kDynamicDecal)) return false;
            if (!runtime.skinInstance) return CaptureRigid(*geometry, out);
            const auto* skin = runtime.skinInstance.get();
            const auto* partitions = skin->skinPartition.get();
            if (!partitions || !skin->skinData || !skin->bones || !skin->numMatrices ||
                skin->numMatrices > 256 || skin->numMatrices > skin->skinData->GetBoneCount() ||
                !skin->skinData->GetBoneDataAddress(0) || !partitions->numPartitions ||
                partitions->numPartitions > kMaxPartitions || partitions->partitions.size() < partitions->numPartitions ||
                out.bones.size() + skin->numMatrices > kMaxBones)
                return false;

            size_t count = 0, indexCount = 0;
            for (uint32_t i = 0; i < partitions->numPartitions; ++i) {
                const auto& part = partitions->partitions[i];
                auto desc = part.vertexDesc;
                const auto stride = desc.GetSize();
                const auto offset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
                if (!part.vertices || !part.triangles || part.strips || !part.numBones || !part.bones ||
                    !part.buffData || !part.buffData->rawVertexData || !part.buffData->rawIndexData ||
                    !desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) ||
                    !desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED) || stride < 28 || stride > 256 ||
                    offset < 16 || offset > stride - 12)
                    return false;
                count += part.vertices; indexCount += size_t(part.triangles) * 3;
            }
            if (out.vertices.size() + count > kMaxVertices || out.indices.size() + indexCount > kMaxIndices)
                return false;

            std::vector<Util::SkinnedVertex> source;
            if (!Util::ReadSkinnedVertices(geometry, source) || source.size() != count)
                return false;

            std::vector<Bone> bones(skin->numMatrices);
            for (uint32_t i = 0; i < skin->numMatrices; ++i) {
                if (!skin->bones[i]) return false;
                const auto transform = skin->bones[i]->world * skin->skinData->GetBoneDataSkinToBone(i);
                bones[i] = Rows(transform);
                if (!Finite(bones[i])) return false;
            }
            const uint32_t boneBase = uint32_t(out.bones.size()), vertexBase = uint32_t(out.vertices.size());
            std::vector<Vertex> vertices;
            vertices.reserve(count);
            for (const auto& v : source) {
                Vertex vertex{ { v.position.x, v.position.y, v.position.z }, {}, v.weight };
                if (!Finite(vertex.position) || !Finite(vertex.weights)) return false;
                float total = 0;
                for (uint32_t i = 0; i < 4; ++i) {
                    if (vertex.weights[i] < 0) return false;
                    if (vertex.weights[i] > 0 && v.bone[i] >= bones.size()) return false;
                    vertex.bones[i] = boneBase + (vertex.weights[i] > 0 ? v.bone[i] : 0);
                    total += vertex.weights[i];
                }
                if (!std::isfinite(total) || std::abs(total - 1.0f) > 0.02f) return false;
                for (auto& weight : vertex.weights) weight /= total;
                vertices.push_back(vertex);
            }

            // Reject a mismatched skin-space transform instead of allowing an
            // incorrect pose to cast shadows elsewhere in the world.
            // The skinned reader emits vertices in exactly this partition order.
            size_t attributeVertex = 0;
            for (uint32_t i = 0; i < partitions->numPartitions; ++i) {
                const auto& part = partitions->partitions[i];
                auto desc = part.vertexDesc;
                for (uint32_t j = 0; j < part.vertices; ++j)
                    CaptureAttributes(desc, part.buffData->rawVertexData + size_t(j) * desc.GetSize(), vertices[attributeVertex++]);
            }
            const auto& bound = geometry->worldBound;
            const size_t step = std::max<size_t>(1, vertices.size() / 32);
            for (size_t i = 0; i < vertices.size(); i += step) {
                const auto& v = vertices[i];
                std::array<float, 3> p{};
                for (uint32_t b = 0; b < 4; ++b) {
                    if (v.weights[b] == 0) continue;
                    const auto& m = bones[v.bones[b] - boneBase];
                    for (uint32_t row = 0; row < 3; ++row)
                        p[row] += v.weights[b] * (m[row * 4] * v.position[0] +
                            m[row * 4 + 1] * v.position[1] + m[row * 4 + 2] * v.position[2] + m[row * 4 + 3]);
                }
                if (!InsideBound(bound, p)) return false;
            }

            std::vector<uint32_t> indices;
            indices.reserve(indexCount);
            uint32_t base = vertexBase;
            for (uint32_t i = 0; i < partitions->numPartitions; ++i) {
                const auto& part = partitions->partitions[i];
                for (uint32_t j = 0; j < uint32_t(part.triangles) * 3; ++j) {
                    const auto index = part.buffData->rawIndexData[j];
                    if (index >= part.vertices) return false;
                    indices.push_back(base + index);
                }
                base += part.vertices;
            }
            out.vertices.insert(out.vertices.end(), vertices.begin(), vertices.end());
            out.bones.insert(out.bones.end(), bones.begin(), bones.end());
            out.indices.insert(out.indices.end(), indices.begin(), indices.end());
            out.primitiveParts.insert(out.primitiveParts.end(), indices.size() / 3, out.parts);
            bool hasColor = true;
            for (uint32_t i = 0; i < partitions->numPartitions; ++i)
                hasColor = hasColor && HasColor(partitions->partitions[i].vertexDesc);
            out.materialSources.push_back(CaptureCharacterMaterial(*geometry, hasColor));
            ++out.parts;
            return true;
        }

        static Snapshot Capture()
        {
            Snapshot result;
            uint32_t actors = 0;
            Util::ForEachLoadedActor([&](RE::Actor* actor) {
                if (!actor || !actor->Is3DLoaded() || actors >= kMaxActors || result.parts >= kMaxParts) return;
                RE::NiPointer<RE::NiAVObject> root{ actor->Get3D(false) };
                if (!root) return;
                ++actors;
                Util::ForEachGeometry(root.get(), [&](RE::BSGeometry* geometry) {
                    if (result.parts < kMaxParts && result.vertices.size() < kMaxVertices &&
                        !CapturePart(geometry, result)) ++result.rejected;
                });
            });
            return result;
        }

        void Initialize(const GraphicsBackend& backend)
        {
            if (pipeline) {
                if (owner.d3d12Device.get() != backend.d3d12Device.get() ||
                    owner.commandQueue.get() != backend.commandQueue.get())
                    throw std::runtime_error("Character caster device changed");
                return;
            }
            auto* device = backend.d3d12Device.get();
            const auto shader = Util::FileHelpers::ReadBoundedBinary(Util::PathHelpers::GetShadersPath() / "DX12NativeScene" / "CharacterSkinning.dxil", 16 * VideoMemoryBudget::kMiB);
            CD3DX12_ROOT_PARAMETER params[4];
            params[0].InitAsShaderResourceView(0);
            params[1].InitAsShaderResourceView(1);
            params[2].InitAsUnorderedAccessView(0);
            params[3].InitAsConstants(1, 0);
            const CD3DX12_ROOT_SIGNATURE_DESC desc(4, params);
            winrt::com_ptr<ID3DBlob> blob, errors;
            winrt::com_ptr<ID3D12RootSignature> newRoot;
            winrt::com_ptr<ID3D12PipelineState> newPipeline;
            winrt::com_ptr<ID3D12Fence> newFence;
            Eval::Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put()));
            Eval::Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(newRoot.put())));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = newRoot.get(); pso.CS = { shader.data(), shader.size() };
            Eval::Check(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(newPipeline.put())));
            Eval::Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(newFence.put())));
            owner = backend; root = std::move(newRoot); pipeline = std::move(newPipeline); completion = std::move(newFence);
        }

    public:
        ~NativeCharacterCasters()
        {
            combined.reset();
            current.reset();
            for (auto& slot : moverSlots)
                if (slot && !GpuFenceRetired(slot->readyFence.get(), slot->readyValue)) {
                    const auto value = slot->readyValue;
                    RetireAfterFences(std::move(slot), owner.d3d12Device, { FencePoint(completion.get(), value) });
                }
            for (auto& slot : slots)
                if (slot && !GpuFenceRetired(slot->readyFence.get(), slot->readyValue)) {
                    const auto value = slot->readyValue;
                    RetireAfterFences(std::move(slot), owner.d3d12Device, { FencePoint(completion.get(), value) });
                }
        }

        // Alan's rigid mover approach: local-space immutable mesh BLASes,
        // independent per-frame instance transforms. Capture missing meshes
        // immediately, so raw engine pointers are never read at submission.
        void ObserveRigidMover(RE::BSGeometry& geometry, uint32_t frame, uint32_t scope)
        {
            if (moverScope != scope) {
                moverMeshes.clear(); moverScope = scope; moverFrame = UINT32_MAX;
            }
            if (moverFrame != frame) {
                movers.clear(); seenMovers.clear(); moverFrame = frame; newMoverMeshes = 0;
            }
            if (movers.size() >= kMaxMovers || std::find(seenMovers.begin(), seenMovers.end(), static_cast<const RE::BSGeometry*>(&geometry)) != seenMovers.end())
                return;
            auto* tri = geometry.AsTriShape();
            const auto& runtime = geometry.GetGeometryRuntimeData();
            if (!tri || geometry.AsDynamicTriShape() || runtime.skinInstance || runtime.alphaProperty ||
                !runtime.rendererData || !runtime.rendererData->rawVertexData || !runtime.rendererData->rawIndexData)
                return;
            auto desc = runtime.vertexDesc;
            const auto counts = tri->GetTrishapeRuntimeData();
            const MeshKey key{ reinterpret_cast<uintptr_t>(runtime.rendererData->rawVertexData),
                reinterpret_cast<uintptr_t>(runtime.rendererData->rawIndexData),
                uintptr_t(counts.vertexCount), uintptr_t(counts.triangleCount), uintptr_t(desc.GetSize()) };
            const auto transform = Rows(geometry.world);
            if (!Finite(transform)) return;
            auto found = moverMeshes.find(key);
            if (found == moverMeshes.end()) {
                // Do not grow a cache of unbounded uploads. At most eight new
                // rigid mesh captures/builds per frame, as in Alan's path.
                if (moverMeshes.size() >= kMaxCachedMeshes || newMoverMeshes >= kMaxNewMovers) return;
                Snapshot snapshot;
                if (!CapturePart(&geometry, snapshot) || snapshot.parts != 1) return;
                auto mesh = std::make_shared<RigidMesh>();
                mesh->geometry = RE::NiPointer<RE::BSGeometry>(&geometry);
                mesh->positions.reserve(snapshot.vertices.size());
                for (const auto& vertex : snapshot.vertices) mesh->positions.push_back(vertex.position);
                mesh->indices = std::move(snapshot.indices);
                found = moverMeshes.emplace(key, std::move(mesh)).first;
                ++newMoverMeshes;
            }
            found->second->lastFrame = frame;
            seenMovers.push_back(&geometry);
            movers.push_back({ found->second, transform });
        }

        void Reset()
        {
            combined.reset(); current.reset(); materialWorker.Reset();
            attemptedFrame = UINT32_MAX; attemptedScope = 0;
            moverFrame = UINT32_MAX; moverScope = 0; movers.clear(); seenMovers.clear(); moverMeshes.clear();
        }
        std::shared_ptr<const CharacterCasterFrame> Get(uint32_t frame, uint32_t scope) const
        {
            if (combined && combined->Submitted(frame, scope)) return combined;
            return current && current->Submitted(frame, scope) ? current : nullptr;
        }
        const std::string& Failure() const { return failure; }
        uint32_t RejectedParts() const { return rejectedParts; }

        void Publish(const GraphicsBackend& backend, uint32_t frame, uint32_t scope, uint32_t interval)
        {
            if (!scope || (attemptedFrame == frame && attemptedScope == scope)) return;
            attemptedFrame = frame; attemptedScope = scope;
            combined.reset();
            for (auto& slot : moverSlots) {
                if (slot && slot->readyValue == UINT64_MAX && completion && completion->GetCompletedValue() != UINT64_MAX) {
                    const auto rescue = ++nextFence;
                    if (SUCCEEDED(backend.commandQueue->Signal(completion.get(), rescue))) slot->readyValue = rescue;
                }
                if (slot && slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue)) {
                    slot->actorSource.reset(); slot->rigidResources.clear();
                }
            }
            const bool reuseActor = interval == 2u && current && current->allowedAge == 1u &&
                frame - current->frame == 1u && current->Submitted(frame, scope);
            if (!reuseActor) PublishActor(backend, frame, scope, interval);
            PublishMovers(backend, frame, scope);
            std::erase_if(moverMeshes, [&](const auto& entry) { return frame - entry.second->lastFrame > kMoverRetentionFrames; });
        }

    private:
        void PublishActor(const GraphicsBackend& backend, uint32_t frame, uint32_t scope, uint32_t interval)
        {
            current.reset();
            std::shared_ptr<CharacterCasterFrame>* selected = nullptr;
            bool reusedUnsubmitted = false;
            try {
                Initialize(backend);
                if (completion->GetCompletedValue() == UINT64_MAX) return;
                for (auto& slot : slots) {
                    if (slot && slot->readyValue == UINT64_MAX) {
                        const auto rescue = ++nextFence;
                        if (SUCCEEDED(backend.commandQueue->Signal(completion.get(), rescue))) slot->readyValue = rescue;
                    }
                    if (!selected && (!slot || (slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue))))
                        selected = &slot;
                }
                if (!selected) { failure = "Character caster frame pool busy"; return; }
                auto snapshot = Capture();
                rejectedParts = snapshot.rejected;
                if (snapshot.vertices.empty() || snapshot.indices.empty()) { failure = "No admitted character parts"; return; }
                auto device = backend.d3d12Device.try_as<ID3D12Device5>();
                if (!device) throw std::runtime_error("DXR device unavailable");
                D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
                geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                geometry.Triangles.VertexCount = UINT(snapshot.vertices.size());
                geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                geometry.Triangles.VertexBuffer.StrideInBytes = CharacterCasterFrame::kPosedVertexStride;
                geometry.Triangles.IndexCount = UINT(snapshot.indices.size());
                geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottomInput{};
                bottomInput.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                bottomInput.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                bottomInput.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
                bottomInput.NumDescs = 1; bottomInput.pGeometryDescs = &geometry;
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topInput{};
                topInput.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
                topInput.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                topInput.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
                topInput.NumDescs = 1;
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottomInfo{}, topInfo{};
                device->GetRaytracingAccelerationStructurePrebuildInfo(&bottomInput, &bottomInfo);
                device->GetRaytracingAccelerationStructurePrebuildInfo(&topInput, &topInfo);
                if (!bottomInfo.ResultDataMaxSizeInBytes || !topInfo.ResultDataMaxSizeInBytes ||
                    !bottomInfo.ScratchDataSizeInBytes || !topInfo.ScratchDataSizeInBytes)
                    throw std::runtime_error("Character caster AS unsupported");
                const uint64_t scratchBytes = std::max(bottomInfo.ScratchDataSizeInBytes, topInfo.ScratchDataSizeInBytes);
                const uint64_t positionBytes = snapshot.vertices.size() * uint64_t(CharacterCasterFrame::kPosedVertexStride);
                const uint32_t triangleCount = uint32_t(snapshot.indices.size() / 3);
                if (snapshot.primitiveParts.size() != triangleCount) throw std::runtime_error("Character primitive mapping mismatch");
                snapshot.indices.insert(snapshot.indices.end(), snapshot.primitiveParts.begin(), snapshot.primitiveParts.end());
                uint64_t required = 0;
                const auto add = [&](uint64_t bytes, D3D12_RESOURCE_FLAGS flags) {
                    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
                    const auto size = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
                    if (!size || size == UINT64_MAX || size > UINT64_MAX - required)
                        throw std::runtime_error("Character caster allocation unsupported");
                    required += size;
                };
                constexpr auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                add(bottomInfo.ResultDataMaxSizeInBytes, uav); add(topInfo.ResultDataMaxSizeInBytes, uav);
                add(scratchBytes, uav); add(positionBytes, uav);
                add(snapshot.vertices.size() * sizeof(Vertex), D3D12_RESOURCE_FLAG_NONE);
                add(snapshot.bones.size() * sizeof(Bone), D3D12_RESOURCE_FLAG_NONE);
                add(snapshot.indices.size() * sizeof(uint32_t), D3D12_RESOURCE_FLAG_NONE);
                add(sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_RESOURCE_FLAG_NONE);
                const auto fits = [](const auto& resource, uint64_t bytes) {
                    return resource && resource->GetDesc().Width >= bytes;
                };
                const bool reuse = *selected && (*selected)->allocator && (*selected)->commands &&
                    fits((*selected)->bottom, bottomInfo.ResultDataMaxSizeInBytes) && fits((*selected)->acceleration, topInfo.ResultDataMaxSizeInBytes) &&
                    fits((*selected)->scratch, scratchBytes) && fits((*selected)->positions, positionBytes) &&
                    fits((*selected)->bind, snapshot.vertices.size() * sizeof(Vertex)) && fits((*selected)->palette, snapshot.bones.size() * sizeof(Bone)) &&
                    fits((*selected)->indices, snapshot.indices.size() * sizeof(uint32_t)) && fits((*selected)->instance, sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
                uint64_t tracked = 0;
                for (const auto& slot : slots) if (slot) tracked += slot->allocationBytes;
                const auto allowance = budget.Query(device.get(), tracked);
                if (!reuse && (!allowance.localBudget || required > allowance.effectiveBytes ||
                    tracked > allowance.effectiveBytes - required)) { failure = "Character caster DXGI budget deferred"; return; }

                reusedUnsubmitted = reuse;
                auto next = reuse ? *selected : std::make_shared<CharacterCasterFrame>();
                next->frame = frame; next->scope = scope; next->allowedAge = interval == 2u ? 1u : 0u; next->parts = snapshot.parts;
                next->vertices = uint32_t(snapshot.vertices.size()); next->triangles = triangleCount;
                if (snapshot.materialSources.size() != snapshot.parts) throw std::runtime_error("Character material part mapping mismatch");
                next->materials = materialWorker.Update(backend, frame, scope, std::move(snapshot.materialSources));
                if (!reuse) next->allocationBytes = required;
                next->root = root; next->pipeline = pipeline;
                next->readyFence = completion;
                if (!reuse) {
                    next->bind = Eval::Table(device.get(), snapshot.vertices);
                    next->palette = Eval::Table(device.get(), snapshot.bones);
                    next->indices = Eval::Table(device.get(), snapshot.indices);
                    const auto buffer = [&](uint64_t bytes, D3D12_RESOURCE_STATES state) {
                        return Eval::Buffer(device.get(), bytes, D3D12_HEAP_TYPE_DEFAULT, state, uav);
                    };
                    next->positions = buffer(positionBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    next->scratch = buffer(scratchBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    next->bottom = buffer(bottomInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
                    next->acceleration = buffer(topInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
                } else {
                    UpdateTable(next->bind.get(), snapshot.vertices);
                    UpdateTable(next->palette.get(), snapshot.bones);
                    UpdateTable(next->indices.get(), snapshot.indices);
                }
                D3D12_RAYTRACING_INSTANCE_DESC instance{};
                instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1;
                instance.InstanceMask = 0xff; instance.AccelerationStructure = next->bottom->GetGPUVirtualAddress();
                if (reuse) {
                    UpdateTable(next->instance.get(), std::vector{ instance });
                    Eval::Check(next->allocator->Reset());
                    Eval::Check(next->commands->Reset(next->allocator.get(), pipeline.get()));
                } else {
                    next->instance = Eval::Table(device.get(), std::vector{ instance });
                    Eval::Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(next->allocator.put())));
                    Eval::Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, next->allocator.get(), pipeline.get(), IID_PPV_ARGS(next->commands.put())));
                }
                auto* cmd = next->commands.get();
                if (reuse) {
                    const auto writable = CD3DX12_RESOURCE_BARRIER::Transition(next->positions.get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    cmd->ResourceBarrier(1, &writable);
                }
                cmd->SetComputeRootSignature(root.get());
                cmd->SetComputeRootShaderResourceView(0, next->bind->GetGPUVirtualAddress());
                cmd->SetComputeRootShaderResourceView(1, next->palette->GetGPUVirtualAddress());
                cmd->SetComputeRootUnorderedAccessView(2, next->positions->GetGPUVirtualAddress());
                cmd->SetComputeRoot32BitConstant(3, next->vertices, 0);
                cmd->Dispatch((next->vertices + 63) / 64, 1, 1);
                auto positionsReady = CD3DX12_RESOURCE_BARRIER::Transition(next->positions.get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                cmd->ResourceBarrier(1, &positionsReady);
                geometry.Triangles.VertexBuffer.StartAddress = next->positions->GetGPUVirtualAddress();
                geometry.Triangles.IndexBuffer = next->indices->GetGPUVirtualAddress();
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
                build.Inputs = bottomInput; build.DestAccelerationStructureData = next->bottom->GetGPUVirtualAddress();
                build.ScratchAccelerationStructureData = next->scratch->GetGPUVirtualAddress();
                cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
                const std::array barriers{ CD3DX12_RESOURCE_BARRIER::UAV(next->bottom.get()), CD3DX12_RESOURCE_BARRIER::UAV(next->scratch.get()) };
                cmd->ResourceBarrier(UINT(barriers.size()), barriers.data());
                build.Inputs = topInput; build.Inputs.InstanceDescs = next->instance->GetGPUVirtualAddress();
                build.DestAccelerationStructureData = next->acceleration->GetGPUVirtualAddress();
                cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
                const auto ready = CD3DX12_RESOURCE_BARRIER::UAV(next->acceleration.get());
                cmd->ResourceBarrier(1, &ready);
                Eval::Check(cmd->Close());
                next->readyValue = UINT64_MAX; *selected = next;
                reusedUnsubmitted = false;
                ID3D12CommandList* lists[]{cmd};
                backend.commandQueue->ExecuteCommandLists(1, lists);
                const auto value = ++nextFence;
                Eval::Check(backend.commandQueue->Signal(completion.get(), value));
                next->readyValue = value; current = std::move(next); failure.clear();
            } catch (const std::exception& e) {
                // A failed recording may leave the reused command list open. The
                // old GPU work has retired; discard that slot rather than reset it again.
                if (reusedUnsubmitted && selected) selected->reset();
                failure = e.what();
            }
        }

        void PublishMovers(const GraphicsBackend& backend, uint32_t frame, uint32_t scope)
        {
            if (moverFrame != frame || moverScope != scope || movers.empty()) return;
            try {
                Initialize(backend);
                auto device = backend.d3d12Device.try_as<ID3D12Device5>();
                if (!device || completion->GetCompletedValue() == UINT64_MAX) return;
                auto selected = std::ranges::find_if(moverSlots, [](const auto& slot) {
                    return !slot || (slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue));
                });
                if (selected == moverSlots.end()) return;
                auto next = *selected ? *selected : std::make_shared<CharacterCasterFrame>();
                next->frame = frame; next->scope = scope; next->allowedAge = 0;
                next->actorSource = current && current->Submitted(frame, scope) ? current : nullptr;
                next->parts = next->actorSource ? next->actorSource->parts : 0;
                next->vertices = next->actorSource ? next->actorSource->vertices : 0;
                next->triangles = next->actorSource ? next->actorSource->triangles : 0;
                next->rigidResources.clear();
                next->rigidInstances = 0;
                constexpr auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                const auto makeBuffer = [&](uint64_t bytes, D3D12_RESOURCE_STATES state, D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT) {
                    const auto flags = heap == D3D12_HEAP_TYPE_DEFAULT ? uav : D3D12_RESOURCE_FLAG_NONE;
                    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
                    const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
                    if (!allocation || allocation == UINT64_MAX || allocation > budget.Query(device.get(), 0).effectiveBytes)
                        throw std::runtime_error("Rigid caster allocation exceeds DXGI allowance");
                    return Eval::Buffer(device.get(), bytes, heap, state, flags);
                };
                if (!next->allocator) {
                    Eval::Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(next->allocator.put())));
                    Eval::Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, next->allocator.get(), nullptr, IID_PPV_ARGS(next->commands.put())));
                } else {
                    Eval::Check(next->allocator->Reset());
                    Eval::Check(next->commands->Reset(next->allocator.get(), nullptr));
                }
                // Publish the slot before recording. A failed submission is
                // never reused until a rescue fence retires the recorded work.
                *selected = next;
                auto* cmd = next->commands.get();
                std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
                instances.reserve(movers.size() + 1);
                if (next->actorSource) {
                    D3D12_RAYTRACING_INSTANCE_DESC instance{};
                    instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1;
                    instance.InstanceMask = 0xff;
                    instance.AccelerationStructure = next->actorSource->bottom->GetGPUVirtualAddress();
                    instances.push_back(instance);
                }
                std::vector<std::pair<std::shared_ptr<RigidMesh>, winrt::com_ptr<ID3D12Resource>>> built;
                for (const auto& mover : movers) {
                    auto& mesh = *mover.mesh;
                    auto blas = mesh.blas;
                    if (!blas) {
                        const auto pending = std::ranges::find_if(built, [&](const auto& entry) { return entry.first == mover.mesh; });
                        if (pending != built.end()) blas = pending->second;
                        else {
                            if (built.size() >= kMaxNewMovers || mesh.positions.empty() || mesh.indices.empty()) continue;
                            auto positions = Eval::Table(device.get(), mesh.positions);
                            auto indices = Eval::Table(device.get(), mesh.indices);
                            D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
                            geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                            geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                            geometry.Triangles.VertexBuffer = { positions->GetGPUVirtualAddress(), sizeof(mesh.positions.front()) };
                            geometry.Triangles.VertexCount = UINT(mesh.positions.size());
                            geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                            geometry.Triangles.IndexBuffer = indices->GetGPUVirtualAddress();
                            geometry.Triangles.IndexCount = UINT(mesh.indices.size());
                            geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
                            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
                            input.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                            input.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
                            input.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; input.NumDescs = 1; input.pGeometryDescs = &geometry;
                            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
                            device->GetRaytracingAccelerationStructurePrebuildInfo(&input, &info);
                            if (!info.ResultDataMaxSizeInBytes || !info.ScratchDataSizeInBytes) continue;
                            blas = makeBuffer(info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
                            auto scratch = makeBuffer(info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
                            build.Inputs = input; build.DestAccelerationStructureData = blas->GetGPUVirtualAddress();
                            build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
                            next->rigidResources.insert(next->rigidResources.end(), { positions, indices, scratch, blas });
                            cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
                            const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(blas.get());
                            cmd->ResourceBarrier(1, &barrier);
                            built.emplace_back(mover.mesh, blas);
                        }
                    }
                    next->rigidResources.push_back(blas);
                    D3D12_RAYTRACING_INSTANCE_DESC instance{};
                    std::memcpy(instance.Transform, mover.transform.data(), sizeof(instance.Transform));
                    instance.InstanceID = UINT(instances.size()); instance.InstanceMask = 0xff;
                    instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
                    instance.AccelerationStructure = blas->GetGPUVirtualAddress();
                    instances.push_back(instance); ++next->rigidInstances;
                }
                if (!next->rigidInstances) { Eval::Check(cmd->Close()); return; }
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};
                top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
                top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
                top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; top.NumDescs = kMaxMovers + 1;
                if (!next->acceleration || !next->scratch || !next->instance) {
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
                    device->GetRaytracingAccelerationStructurePrebuildInfo(&top, &info);
                    if (!info.ResultDataMaxSizeInBytes || !info.ScratchDataSizeInBytes) throw std::runtime_error("Rigid TLAS unsupported");
                    next->acceleration = makeBuffer(info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
                    next->scratch = makeBuffer(info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    next->instance = makeBuffer(uint64_t(kMaxMovers + 1) * sizeof(instances.front()), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD);
                }
                UpdateTable(next->instance.get(), instances);
                top.NumDescs = UINT(instances.size()); top.InstanceDescs = next->instance->GetGPUVirtualAddress();
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
                build.Inputs = top; build.DestAccelerationStructureData = next->acceleration->GetGPUVirtualAddress();
                build.ScratchAccelerationStructureData = next->scratch->GetGPUVirtualAddress();
                cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
                const auto ready = CD3DX12_RESOURCE_BARRIER::UAV(next->acceleration.get());
                cmd->ResourceBarrier(1, &ready);
                Eval::Check(cmd->Close());
                next->readyFence = completion; next->readyValue = UINT64_MAX;
                ID3D12CommandList* lists[]{ cmd };
                backend.commandQueue->ExecuteCommandLists(1, lists);
                const auto value = ++nextFence;
                Eval::Check(backend.commandQueue->Signal(completion.get(), value));
                next->readyValue = value;
                for (auto& [mesh, blas] : built) {
                    mesh->blas = std::move(blas);
                    mesh->positions.clear(); mesh->indices.clear();
                }
                combined = std::move(next);
            } catch (const std::exception& error) {
                failure = error.what();
                // Drop failed, unsubmitted recording contexts. Submitted but
                // untracked ones stay quarantined until a later rescue signal or device loss.
                for (auto& slot : moverSlots)
                    if (slot && slot->frame == frame && slot->readyValue != UINT64_MAX) slot.reset();
            }
        }

        static constexpr uint32_t kMaxMovers = 512, kMaxNewMovers = 8, kMaxCachedMeshes = 512, kMoverRetentionFrames = 300;
        using MeshKey = std::array<uintptr_t, 5>;
        struct RigidMesh
        {
            RE::NiPointer<RE::BSGeometry> geometry;
            std::vector<std::array<float, 3>> positions;
            std::vector<uint32_t> indices;
            winrt::com_ptr<ID3D12Resource> blas;
            uint32_t lastFrame = 0;
        };
        struct RigidInstance { std::shared_ptr<RigidMesh> mesh; Bone transform; };
        std::map<MeshKey, std::shared_ptr<RigidMesh>> moverMeshes;
        std::vector<RigidInstance> movers;
        std::array<std::shared_ptr<CharacterCasterFrame>, 4> moverSlots;
        std::shared_ptr<CharacterCasterFrame> combined;
        uint32_t moverScope = 0, newMoverMeshes = 0;
        GraphicsBackend owner;
        CharacterMaterialWorker materialWorker;
        VideoMemoryBudget budget;
        std::array<std::shared_ptr<CharacterCasterFrame>, 3> slots;
        std::shared_ptr<CharacterCasterFrame> current;
        winrt::com_ptr<ID3D12RootSignature> root;
        winrt::com_ptr<ID3D12PipelineState> pipeline;
        winrt::com_ptr<ID3D12Fence> completion;
        uint64_t nextFence = 0;
        uint32_t attemptedFrame = UINT32_MAX, attemptedScope = 0, rejectedParts = 0;
        uint32_t moverFrame = UINT32_MAX;
        std::vector<const RE::BSGeometry*> seenMovers;
        std::string failure;
    };
}

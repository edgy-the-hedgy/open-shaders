#pragma once

#include "DX12GpuRetirement.h"
#include "GraphicsBackend.h"
#include "Globals.h"
#include "RE/B/BSLightingShaderProperty.h"
#include "DX12NativeMaterialEvaluation.h"
#include "DX12CharacterMaterialWorker.h"
#include "VideoMemoryBudget.h"
#include "SystemMemoryBudget.h"
#include "CharacterPreparationPolicy.h"
#include "Utils/ActorUtils.h"
#include "Utils/FileSystem.h"
#include "Utils/SkinnedMesh.h"
#include "Utils/TracyZones.h"

#include <DirectXPackedVector.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
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
    struct RigidRecordedBuild
    {
        winrt::com_ptr<ID3D12Resource> positions, indices, scratch, blas;
        winrt::com_ptr<ID3D12CommandAllocator> allocator;
        winrt::com_ptr<ID3D12GraphicsCommandList4> commands;
        uint64_t allocationBytes = 0;
    };

    struct CharacterCasterFrame
    {
        uint32_t frame = UINT32_MAX, scope = 0, parts = 0, vertices = 0, triangles = 0;
        uint64_t allocationBytes = 0, readyValue = 0;
        uint32_t allowedAge = 0, rigidInstances = 0;
        // frame is the source pose frame for actors, never the submission
        // frame. Combined frames retain the independent actorSource stamp.
        uint32_t submittedFrame = UINT32_MAX, captureInterval = 1;
        uint64_t poseRevision = 0;
        int64_t capturedQpc = 0;
        static constexpr uint32_t kPosedVertexStride = MaterialFrameContract::kCharacterPosedVertexStride;
        std::shared_ptr<const CharacterCasterFrame> actorSource;
        std::shared_ptr<const CharacterMaterialPublication> materials;
        std::shared_ptr<const std::vector<DX12::PreparedMaterialSource>> materialSources;
        winrt::com_ptr<ID3D12Resource> rigidShading;
        uint32_t rigidShadingCount = 0;
        std::vector<winrt::com_ptr<ID3D12Resource>> rigidResources;
        std::vector<std::shared_ptr<const RigidRecordedBuild>> rigidBuilds;
        winrt::com_ptr<ID3D12Fence> readyFence;
        winrt::com_ptr<ID3D12Resource> acceleration, bottom, positions, scratch, bind, palette, indices, instance;
        winrt::com_ptr<ID3D12CommandAllocator> allocator;
        winrt::com_ptr<ID3D12GraphicsCommandList4> commands;
        winrt::com_ptr<ID3D12RootSignature> root;
        winrt::com_ptr<ID3D12PipelineState> pipeline;

        struct ReflectionView
        {
            const CharacterCasterFrame* actor = nullptr;
            const NativeMaterialEvaluation* evaluation = nullptr;
            ID3D12Resource* movers = nullptr;
            uint32_t moverCount = 0;
        };

        ReflectionView Reflections() const
        {
            ReflectionView view;
            const auto* actor = actorSource ? actorSource.get() : (rigidInstances ? nullptr : this);
            const bool actorGeometry = actor && actor->scope == scope && actor->positions && actor->indices &&
                MaterialFrameContract::ValidCharacterPrimitiveLayout(actor->vertices, actor->triangles, actor->parts,
                    actor->positions->GetDesc().Width, actor->indices->GetDesc().Width);
            const auto valid = [&](const auto& publication, uint32_t count) {
                return publication && publication->scope == scope && publication->Ready() &&
                    publication->evaluation->instanceCount == count &&
                    publication->evaluation->revision == publication->generation;
            };
            if (rigidShading && rigidShadingCount && rigidShadingCount == rigidInstances &&
                (!actor || actorGeometry) && valid(materials, parts + rigidShadingCount)) {
                view.actor = actorGeometry ? actor : nullptr;
                view.evaluation = materials->evaluation.get();
                view.movers = rigidShading.get();
                view.moverCount = rigidShadingCount;
            } else if (actorGeometry && valid(actor->materials, actor->parts)) {
                view.actor = actor;
                view.evaluation = actor->materials->evaluation.get();
            }
            return view;
        }

        bool Submitted(uint32_t expectedFrame, uint32_t expectedScope) const
        {
            return expectedFrame - frame <= allowedAge && scope == expectedScope && acceleration && readyFence &&
                readyValue != 0 && readyValue != UINT64_MAX && readyFence->GetCompletedValue() != UINT64_MAX &&
                (!actorSource || actorSource->Submitted(expectedFrame, expectedScope));
        }
        const CharacterCasterFrame* Pose() const
        {
            return actorSource ? actorSource.get() : (parts && !rigidInstances ? this : nullptr);
        }
        bool ExactPose(uint32_t expectedFrame) const
        {
            const auto* pose = Pose();
            return pose && pose->frame == expectedFrame;
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

        // Raw engine buffers are copied once while their owner is accessible.
        // No engine pointers cross the task boundary.
        struct ActorPartitionSource
        {
            RE::BSGraphics::VertexDesc desc{};
            std::vector<uint8_t> vertices;
            std::vector<uint16_t> indices, boneMap;
        };
        struct ActorPartSource
        {
            std::vector<ActorPartitionSource> partitions;
            std::vector<Bone> bones;
            RE::NiBound bound{};
            std::shared_ptr<const DX12::PreparedMaterialSource> material;
            bool skinned = false;
        };
        // The reservation survives cancellation while a worker still owns
        // the packet. Queued, running and completed-but-unadopted CPU work
        // share the same bounded allocation policy across scene resets.
        struct PreparationLease
        {
            std::atomic<uint64_t>* counter = nullptr;
            uint64_t bytes = 0;
            explicit PreparationLease(std::atomic<uint64_t>& value) : counter(&value) {}
            PreparationLease(const PreparationLease&) = delete;
            PreparationLease& operator=(const PreparationLease&) = delete;
            ~PreparationLease() { Release(bytes); }
            bool TryGrow(uint64_t count, uint64_t limit)
            {
                if (count > limit || !DX12::HasAvailableSystemMemory(count)) return false;
                auto current = counter->load(std::memory_order_relaxed);
                do {
                    if (current > limit - count) return false;
                } while (!counter->compare_exchange_weak(current, current + count,
                    std::memory_order_acq_rel, std::memory_order_relaxed));
                bytes += count;
                return true;
            }
            void Release(uint64_t count)
            {
                if (count) counter->fetch_sub(count, std::memory_order_acq_rel);
                bytes -= count;
            }
            void TransferTo(PreparationLease& destination)
            {
                // Both leases use the same global reservation counter.
                destination.bytes += bytes;
                bytes = 0;
            }
        };
        inline static std::atomic<uint64_t> actorPreparationBytes{ 0 }, rigidPreparationBytes{ 0 };

        struct ActorSourceSnapshot
        {
            PreparationLease lease{ actorPreparationBytes };
            std::vector<ActorPartSource> parts;
            uint64_t bytes = 0;
            size_t vertices = 0, indices = 0, bones = 0;
            uint32_t rejected = 0;
        };
        static constexpr uint64_t kActorSnapshotBudget = 96ull * VideoMemoryBudget::kMiB;

        static bool CapturePart(RE::BSGeometry* geometry, ActorSourceSnapshot& out)
        {
            if (!geometry || geometry->GetAppCulled() || geometry->AsDynamicTriShape()) return false;
            const auto& runtime = geometry->GetGeometryRuntimeData();
            const auto* property = runtime.shaderProperty.get();
            if (runtime.alphaProperty || !property ||
                property->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get()) return false;
            using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
            using V = RE::BSGraphics::Vertex;
            if (!property->flags.any(Flag::kCastShadows) || property->flags.any(Flag::kVertexAlpha) ||
                property->flags.any(Flag::kScreendoorAlphaFade) || property->flags.any(Flag::kRefraction) ||
                property->flags.any(Flag::kTreeAnim) || property->flags.any(Flag::kDecal) ||
                property->flags.any(Flag::kDynamicDecal)) return false;
            ActorPartSource part;
            part.bound = geometry->worldBound;
            size_t count = 0, indexCount = 0, boneCount = 0;
            uint64_t bytes = 0, working = 0;
            bool hasColor = true, reserved = false, admitted = false;
            const SKSE::stl::scope_exit rollback([&] {
                if (reserved && !admitted) out.lease.Release(working);
            });
            const auto add = [&](size_t vertices, size_t indices, size_t stride, size_t maps) {
                count += vertices; indexCount += indices;
                bytes += uint64_t(vertices) * stride + uint64_t(indices + maps) * sizeof(uint16_t);
            };
            const auto fits = [&] {
                working = bytes + uint64_t(count) * sizeof(Vertex) * 2u +
                    uint64_t(indexCount) * sizeof(uint32_t) * 3u + uint64_t(boneCount) * sizeof(Bone) * 2u;
                return count && indexCount && out.vertices + count <= kMaxVertices &&
                    out.indices + indexCount <= kMaxIndices && out.bones + boneCount <= kMaxBones &&
                    working <= kActorSnapshotBudget && out.bytes <= kActorSnapshotBudget - working &&
                    (reserved = out.lease.TryGrow(working, kActorSnapshotBudget));
            };
            const auto copy = [&](ActorPartitionSource& dest, RE::BSGraphics::VertexDesc desc,
                const uint8_t* vertices, const uint16_t* indices, size_t n, size_t ni) {
                dest.desc = desc;
                dest.vertices.resize(n * desc.GetSize());
                std::memcpy(dest.vertices.data(), vertices, dest.vertices.size());
                dest.indices.assign(indices, indices + ni);
                hasColor = hasColor && HasColor(desc);
            };
            if (!runtime.skinInstance) {
                const auto* tri = geometry->AsTriShape();
                if (!tri || !runtime.rendererData || !runtime.rendererData->rawVertexData ||
                    !runtime.rendererData->rawIndexData) return false;
                auto desc = runtime.vertexDesc;
                const auto stride = desc.GetSize();
                const auto counts = tri->GetTrishapeRuntimeData();
                if (!desc.HasFlag(V::VF_VERTEX) || stride < 16 || stride > 256 || counts.vertexCount > 65535) return false;
                boneCount = 1;
                add(counts.vertexCount, size_t(counts.triangleCount) * 3u, stride, 0);
                if (!fits()) return false;
                auto transform = Rows(geometry->world);
                if (!Finite(transform)) return false;
                part.bones.push_back(transform);
                part.partitions.resize(1);
                copy(part.partitions.front(), desc, runtime.rendererData->rawVertexData,
                    runtime.rendererData->rawIndexData, count, indexCount);
            } else {
                part.skinned = true;
                const auto* skin = runtime.skinInstance.get();
                const auto* partitions = skin->skinPartition.get();
                if (!partitions || !skin->skinData || !skin->bones || !skin->numMatrices ||
                    skin->numMatrices > 256 || skin->numMatrices > skin->skinData->GetBoneCount() ||
                    !skin->skinData->GetBoneDataAddress(0) || !partitions->numPartitions ||
                    partitions->numPartitions > kMaxPartitions || partitions->partitions.size() < partitions->numPartitions) return false;
                boneCount = skin->numMatrices;
                for (uint32_t i = 0; i < partitions->numPartitions; ++i) {
                    const auto& src = partitions->partitions[i];
                    auto desc = src.vertexDesc;
                    const auto stride = desc.GetSize();
                    const auto offset = desc.GetAttributeOffset(V::VA_SKINNING);
                    if (!src.vertices || !src.triangles || src.strips || !src.numBones || !src.bones ||
                        !src.buffData || !src.buffData->rawVertexData || !src.buffData->rawIndexData ||
                        !desc.HasFlag(V::VF_VERTEX) || !desc.HasFlag(V::VF_SKINNED) ||
                        stride < 28 || stride > 256 || offset < 16 || offset > stride - 12) return false;
                    add(src.vertices, size_t(src.triangles) * 3u, stride, src.numBones);
                }
                if (!fits()) return false;
                part.bones.resize(boneCount);
                for (size_t i = 0; i < boneCount; ++i) {
                    if (!skin->bones[i]) return false;
                    part.bones[i] = Rows(skin->bones[i]->world * skin->skinData->GetBoneDataSkinToBone(uint32_t(i)));
                    if (!Finite(part.bones[i])) return false;
                }
                part.partitions.resize(partitions->numPartitions);
                for (uint32_t i = 0; i < partitions->numPartitions; ++i) {
                    const auto& src = partitions->partitions[i];
                    auto& dest = part.partitions[i];
                    copy(dest, src.vertexDesc, src.buffData->rawVertexData, src.buffData->rawIndexData,
                        src.vertices, size_t(src.triangles) * 3u);
                    dest.boneMap.assign(src.bones, src.bones + src.numBones);
                }
            }
            part.material = CaptureCharacterMaterial(*geometry, hasColor);
            out.bytes += working; out.vertices += count; out.indices += indexCount; out.bones += boneCount;
            out.parts.push_back(std::move(part));
            admitted = true;
            return true;
        }

        static void Capture(ActorSourceSnapshot& result)
        {
            ZoneScopedN("DX12::Characters::Capture");
            uint32_t actors = 0;
            Util::ForEachLoadedActor([&](RE::Actor* actor) {
                if (!actor || !actor->Is3DLoaded() || actors >= kMaxActors || result.parts.size() >= kMaxParts) return;
                RE::NiPointer<RE::NiAVObject> root{ actor->Get3D(false) };
                if (!root) return;
                ++actors;
                Util::ForEachGeometry(root.get(), [&](RE::BSGeometry* geometry) {
                    if (result.parts.size() < kMaxParts && result.vertices < kMaxVertices &&
                        !CapturePart(geometry, result)) ++result.rejected;
                });
            });
        }

        static bool DecodePart(const ActorPartSource& part, Snapshot& out)
        {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            const auto boneBase = uint32_t(out.bones.size()), vertexBase = uint32_t(out.vertices.size());
            for (const auto& partition : part.partitions) {
                auto desc = partition.desc;
                const auto stride = desc.GetSize();
                const auto count = partition.vertices.size() / stride;
                const auto base = vertexBase + uint32_t(vertices.size());
                const auto skinOffset = part.skinned ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING) : 0u;
                vertices.reserve(vertices.size() + count);
                for (size_t i = 0; i < count; ++i) {
                    const auto* data = partition.vertices.data() + i * stride;
                    Vertex v{};
                    std::memcpy(v.position.data(), data, sizeof(v.position));
                    if (!Finite(v.position)) return false;
                    if (part.skinned) {
                        uint16_t weights[4]{};
                        std::memcpy(weights, data + skinOffset, sizeof(weights));
                        const auto* local = data + skinOffset + sizeof(weights);
                        float total = 0;
                        for (size_t slot = 0; slot < 4; ++slot) {
                            v.bones[slot] = boneBase;
                            if (local[slot] >= partition.boneMap.size()) continue;
                            const auto bone = partition.boneMap[local[slot]];
                            if (bone >= part.bones.size()) continue;
                            const float weight = DirectX::PackedVector::XMConvertHalfToFloat(weights[slot]);
                            if (!std::isfinite(weight) || weight < 0) return false;
                            v.weights[slot] = weight;
                            v.bones[slot] = boneBase + (weight > 0 ? bone : 0);
                            total += weight;
                        }
                        if (!std::isfinite(total) || std::abs(total - 1.0f) > 0.02f) return false;
                        for (auto& weight : v.weights) weight /= total;
                    } else {
                        v.bones = { boneBase, boneBase, boneBase, boneBase };
                        v.weights = { 1, 0, 0, 0 };
                    }
                    CaptureAttributes(desc, data, v);
                    vertices.push_back(v);
                }
                for (const auto index : partition.indices) {
                    if (index >= count) return false;
                    indices.push_back(base + index);
                }
            }
            const auto step = std::max<size_t>(1, vertices.size() / 32);
            for (size_t i = 0; i < vertices.size(); i += step) {
                const auto& v = vertices[i];
                std::array<float, 3> p{};
                for (uint32_t b = 0; b < 4; ++b) {
                    if (v.weights[b] == 0) continue;
                    const auto& m = part.bones[v.bones[b] - boneBase];
                    for (uint32_t row = 0; row < 3; ++row)
                        p[row] += v.weights[b] * (m[row*4]*v.position[0] + m[row*4+1]*v.position[1] +
                            m[row*4+2]*v.position[2] + m[row*4+3]);
                }
                if (!InsideBound(part.bound, p)) return false;
            }
            out.vertices.insert(out.vertices.end(), vertices.begin(), vertices.end());
            out.bones.insert(out.bones.end(), part.bones.begin(), part.bones.end());
            out.indices.insert(out.indices.end(), indices.begin(), indices.end());
            out.primitiveParts.insert(out.primitiveParts.end(), indices.size() / 3u, out.parts);
            out.materialSources.push_back(part.material ? *part.material : DX12::PreparedMaterialSource{});
            ++out.parts;
            return true;
        }

        static Snapshot DecodeActor(const ActorSourceSnapshot& source)
        {
            ZoneScopedN("DX12::Characters::DecodeSnapshot");
            Snapshot result;
            result.rejected = source.rejected;
            result.vertices.reserve(source.vertices); result.indices.reserve(source.indices);
            result.bones.reserve(source.bones); result.primitiveParts.reserve(source.indices / 3u);
            result.materialSources.reserve(source.parts.size());
            for (const auto& part : source.parts)
                if (!DecodePart(part, result)) ++result.rejected;
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
        void ObserveRigidMover(const GraphicsBackend& backend, RE::BSGeometry& geometry, uint32_t frame, uint32_t scope, uint64_t geometryRevision)
        {
            CS_DRAW_ZONE("DX12::RigidMover::Observe");
            if (moverScope != scope || moverGeometryRevision != geometryRevision) {
                moverMeshes.clear(); moverShadingOrder.clear(); moverShadingCache = nullptr; moverMaterialWorker.Reset();
                moverScope = scope; moverGeometryRevision = geometryRevision; moverFrame = UINT32_MAX;
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
            using V = RE::BSGraphics::Vertex;
            const auto* property = runtime.shaderProperty.get();
            using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
            if (geometry.GetAppCulled() || !property ||
                property->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get() ||
                !property->flags.any(Flag::kCastShadows) || property->flags.any(Flag::kVertexAlpha) ||
                property->flags.any(Flag::kScreendoorAlphaFade) || property->flags.any(Flag::kRefraction) ||
                property->flags.any(Flag::kTreeAnim) || property->flags.any(Flag::kDecal) ||
                property->flags.any(Flag::kDynamicDecal)) return;
            // A cache entry retains this geometry on the engine thread.
            // Do not infer allocation lifetime from shared raw addresses alone.
            const MeshKey key{ reinterpret_cast<uintptr_t>(&geometry), reinterpret_cast<uintptr_t>(runtime.rendererData),
                reinterpret_cast<uintptr_t>(runtime.rendererData->rawVertexData),
                reinterpret_cast<uintptr_t>(runtime.rendererData->rawIndexData),
                uintptr_t(counts.vertexCount), uintptr_t(counts.triangleCount), uintptr_t(desc.GetSize()),
                desc.HasFlag(V::VF_UV) ? uintptr_t(desc.GetAttributeOffset(V::VA_TEXCOORD0)) : UINTPTR_MAX,
                desc.HasFlag(V::VF_NORMAL) ? uintptr_t(desc.GetAttributeOffset(V::VA_NORMAL)) : UINTPTR_MAX,
                desc.HasFlag(V::VF_TANGENT) ? uintptr_t(desc.GetAttributeOffset(V::VA_BINORMAL)) : UINTPTR_MAX,
                desc.HasFlag(V::VF_COLORS) ? uintptr_t(desc.GetAttributeOffset(V::VA_COLOR)) : UINTPTR_MAX };
            const auto transform = Rows(geometry.world);
            if (!Finite(transform)) return;
            auto found = moverMeshes.find(key);
            if (found == moverMeshes.end()) {
                // Do not grow a cache of unbounded uploads. At most eight new
                // rigid mesh captures/builds per frame, as in Alan's path.
                if (moverMeshes.size() >= kMaxCachedMeshes || newMoverMeshes >= kMaxNewMovers) return;
                // Copy the engine-owned arrays while they are accessible.
                // The decoder, shading packer and BLAS recorder consume only
                // the value packet and device reference.
                CS_DRAW_ZONE("DX12::RigidMover::SnapshotAcquisition");
                const uint64_t count = counts.vertexCount, indexCount = uint64_t(counts.triangleCount) * 3u;
                const uint64_t stride = desc.GetSize();
                if (!desc.HasFlag(V::VF_VERTEX) || stride < 16 || stride > 256 ||
                    !count || !indexCount || count > 65535 || indexCount > kMaxIndices) return;
                uint64_t retained = 0, pendingBytes = 0;
                for (const auto& entry : moverMeshes) {
                    retained += entry.second->reservedShadingBytes;
                    if (entry.second->pending) pendingBytes += entry.second->pending->snapshotBytes;
                }
                const uint64_t sourceBytes = count * stride + indexCount * sizeof(uint16_t);
                const uint64_t bytes = count * CharacterCasterFrame::kPosedVertexStride + indexCount * sizeof(uint32_t);
                const bool withShading = bytes <= kMoverShadingBudget && retained <= kMoverShadingBudget - bytes;
                const uint64_t workingBytes = sourceBytes + count * sizeof(std::array<float, 3>) +
                    indexCount * sizeof(uint32_t) + (withShading ? bytes : 0);
                if (workingBytes > kMoverPendingBudget || pendingBytes > kMoverPendingBudget - workingBytes) {
                    ++deferredRigidPacks; return;
                }
                const auto gate = globals::shaderCache ? globals::shaderCache->GetRuntimeTaskGate() : nullptr;
                if (!gate || !backend.d3d12Device) { ++deferredRigidPacks; return; }
                const auto reservation = gate->TryReserve(Util::RuntimePreparationLane::Rigid);
                if (!reservation) { ++deferredRigidPacks; ++rigidCaptureAdmissionSkips; return; }
                const SKSE::stl::scope_exit cancelReservation([&] { gate->CompleteReservation(reservation); });
                auto job = std::make_shared<RigidPackJob>();
                if (!job->lease.TryGrow(workingBytes, kMoverPendingBudget)) { ++deferredRigidPacks; return; }
                auto packet = std::make_unique<RigidSourcePacket>();
                packet->desc = desc; packet->transform = transform; packet->bound = geometry.worldBound;
                packet->vertices.resize(size_t(count * stride));
                std::memcpy(packet->vertices.data(), runtime.rendererData->rawVertexData, packet->vertices.size());
                packet->indices.assign(runtime.rendererData->rawIndexData, runtime.rendererData->rawIndexData + indexCount);
                auto mesh = std::make_shared<RigidMesh>();
                mesh->geometry = RE::NiPointer<RE::BSGeometry>(&geometry);
                mesh->reservedShadingBytes = withShading ? bytes : 0;
                mesh->pending = job;
                job->snapshotBytes = workingBytes;
                job->packet = std::move(packet);
                std::weak_ptr<RigidPackJob> weakJob = job;
                if (gate->CompleteReservation(reservation, [weakJob, device = backend.d3d12Device, withShading] {
                    if (const auto retainedJob = weakJob.lock())
                        PackRigidMesh(retainedJob, device, withShading);
                })) ++asyncRigidPacks;
                else { ++deferredRigidPacks; return; }
                found = moverMeshes.emplace(key, std::move(mesh)).first;
                ++newMoverMeshes;
            }
            auto& mesh = *found->second;
            mesh.lastFrame = frame;
            if (mesh.pending) {
                if (!mesh.pending->finished.load(std::memory_order_acquire)) return;
                if (!mesh.pending->result) {
                    moverMeshes.erase(found);
                    return;
                }
                auto& packed = *mesh.pending->result;
                mesh.recorded = std::move(packed.recorded);
                mesh.shading = std::move(packed.shading);
                mesh.shadingIndices = std::move(packed.shadingIndices);
                mesh.pending.reset();
            }
            seenMovers.push_back(&geometry);
            movers.push_back({ found->second, transform, CaptureCharacterMaterial(geometry, HasColor(desc)) });
        }

        void Reset()
        {
            combined.reset(); current.reset(); materialWorker.Reset(); moverMaterialWorker.Reset();
            moverShadingOrder.clear(); moverShadingCache = nullptr;
            attemptedFrame = UINT32_MAX; attemptedScope = 0;
            moverFrame = UINT32_MAX; moverScope = 0; moverGeometryRevision = 0;
            movers.clear(); seenMovers.clear(); moverMeshes.clear();
            if (actorJob) ++cancelledActorRecords;
            actorJob.reset();
            captureFrame = UINT32_MAX; captureScope = 0;
            lastActorCaptureFrame = UINT32_MAX; lastActorCaptureScope = 0;
        }
        std::shared_ptr<const CharacterCasterFrame> Get(uint32_t frame, uint32_t scope) const
        {
            if (combined && combined->Submitted(frame, scope)) return combined;
            return current && current->Submitted(frame, scope) ? current : nullptr;
        }
        const std::string& Failure() const { return failure; }
        uint32_t RejectedParts() const { return rejectedParts; }
        uint64_t AsyncRigidPacks() const { return asyncRigidPacks; }
        uint64_t InlineRigidPacks() const { return inlineRigidPacks; }
        uint64_t DeferredRigidPacks() const { return deferredRigidPacks; }
        uint64_t AsyncActorRecords() const { return asyncActorRecords; }
        uint64_t DeferredActorRecords() const { return deferredActorRecords; }
        uint64_t StaleActorRecords() const { return staleActorRecords; }
        uint64_t CancelledActorRecords() const { return cancelledActorRecords; }
        uint64_t ApproximateActorRecords() const { return approximateActorRecords; }
        uint64_t ActorCaptureAdmissionSkips() const { return actorCaptureAdmissionSkips; }
        uint64_t RigidCaptureAdmissionSkips() const { return rigidCaptureAdmissionSkips; }
        uint64_t PublishedActorRecords() const { return publishedActorRecords; }
        uint64_t ActorSnapshotBytes() const { return actorJob ? actorJob->snapshotBytes : 0; }
        static uint64_t PendingActorBytes() { return actorPreparationBytes.load(std::memory_order_relaxed); }
        static uint64_t PendingRigidBytes() { return rigidPreparationBytes.load(std::memory_order_relaxed); }
        uint64_t ResidentActorBytes() const
        {
            uint64_t bytes = 0;
            for (const auto& slot : slots) if (slot) bytes += slot->allocationBytes;
            return bytes;
        }
        uint64_t ResidentRigidShadingBytes() const
        {
            uint64_t bytes = 0;
            for (const auto& [key, mesh] : moverMeshes)
                bytes += mesh->shading.size() * sizeof(float) + mesh->shadingIndices.size() * sizeof(uint32_t);
            return bytes;
        }

        void Publish(const GraphicsBackend& backend, uint32_t frame, uint32_t scope, uint32_t interval, uint64_t geometryRevision)
        {
            ZoneScopedN("DX12::Characters::Publish");
            if (!scope || (attemptedFrame == frame && attemptedScope == scope)) return;
            attemptedFrame = frame; attemptedScope = scope;
            combined.reset();
            for (auto& slot : moverSlots) {
                if (slot && slot->readyValue == UINT64_MAX && completion && completion->GetCompletedValue() != UINT64_MAX) {
                    const auto rescue = ++nextFence;
                    if (SUCCEEDED(backend.commandQueue->Signal(completion.get(), rescue))) slot->readyValue = rescue;
                }
                if (slot && slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue)) {
                    slot->actorSource.reset(); slot->rigidResources.clear(); slot->rigidBuilds.clear();
                    slot->materials.reset(); slot->rigidShading = nullptr;
                }
            }
            // A missing early callback may still submit a snapshot here;
            // never wait for it or promote a stale recording.
            PrepareActor(backend, frame, scope, interval);
            CollectActor(backend, frame, scope, interval);
            if (moverGeometryRevision == geometryRevision)
                PublishMovers(backend, frame, scope);
            std::erase_if(moverMeshes, [&](const auto& entry) { return frame - entry.second->lastFrame > kMoverRetentionFrames; });
        }

    private:
        struct ActorRecordJob
        {
            std::atomic<bool> finished{ false };
            std::shared_ptr<CharacterCasterFrame> result;
            std::string failure;
            uint32_t frame = 0, scope = 0, interval = 1, slot = 0, rejected = 0;
            uint64_t snapshotBytes = 0, poseRevision = 0;
            int64_t capturedQpc = 0;
            PreparationLease lease{ actorPreparationBytes };
            std::unique_ptr<ActorSourceSnapshot> snapshot;
        };

        // Only owned CPU arrays, a retired slot and D3D12 COM objects enter
        // this function. It records but never executes a command list.
        static std::shared_ptr<CharacterCasterFrame> RecordActor(Snapshot& snapshot,
            std::shared_ptr<CharacterCasterFrame> selected, uint64_t tracked,
            const winrt::com_ptr<ID3D12Device>& device12,
            const winrt::com_ptr<ID3D12RootSignature>& root,
            const winrt::com_ptr<ID3D12PipelineState>& pipeline,
            const winrt::com_ptr<ID3D12Fence>& completion,
            uint32_t frame, uint32_t scope, uint32_t interval)
        {
            ZoneScopedN("DX12::Characters::RecordAcceleration");
                auto device = device12.try_as<ID3D12Device5>();
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
                const bool reuse = selected && selected->allocator && selected->commands &&
                    fits(selected->bottom, bottomInfo.ResultDataMaxSizeInBytes) && fits(selected->acceleration, topInfo.ResultDataMaxSizeInBytes) &&
                    fits(selected->scratch, scratchBytes) && fits(selected->positions, positionBytes) &&
                    fits(selected->bind, snapshot.vertices.size() * sizeof(Vertex)) && fits(selected->palette, snapshot.bones.size() * sizeof(Bone)) &&
                    fits(selected->indices, snapshot.indices.size() * sizeof(uint32_t)) && fits(selected->instance, sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
                VideoMemoryBudget budget;
                const auto allowance = budget.Query(device.get(), tracked);
                if (!reuse && (!allowance.localBudget || required > allowance.effectiveBytes ||
                    tracked > allowance.effectiveBytes - required))
                    throw std::runtime_error("Character caster DXGI budget deferred");

                auto next = reuse ? std::move(selected) : std::make_shared<CharacterCasterFrame>();
                next->frame = frame; next->scope = scope; next->allowedAge = ActorMaxPoseAge(interval); next->parts = snapshot.parts;
                next->captureInterval = ActorCaptureInterval(interval); next->submittedFrame = UINT32_MAX;
                next->vertices = uint32_t(snapshot.vertices.size()); next->triangles = triangleCount;
                if (snapshot.materialSources.size() != snapshot.parts) throw std::runtime_error("Character material part mapping mismatch");
                next->materialSources = std::make_shared<const std::vector<DX12::PreparedMaterialSource>>(std::move(snapshot.materialSources));
                next->materials.reset();
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
            return next;
        }

        void PruneActor(uint32_t frame, uint32_t scope, uint32_t interval)
        {
            if (current && (!current->Submitted(frame, scope) ||
                !ActorRequestCompatible(current->scope, current->captureInterval, scope, interval)))
                current.reset();
            // Do not cancel a running job merely for crossing a frame boundary.
            // It must finish (or become incompatible with the scene or mode).
            if (actorJob && !ActorRequestCompatible(actorJob->scope, actorJob->interval, scope, interval)) {
                actorJob.reset();
                ++cancelledActorRecords;
            }
        }

        void CollectActor(const GraphicsBackend& backend, uint32_t frame, uint32_t scope, uint32_t interval)
        {
            PruneActor(frame, scope, interval);
            if (!actorJob || !actorJob->finished.load(std::memory_order_acquire)) return;
            auto job = std::move(actorJob);
            if (!ActorPoseAgeAllowed(frame, job->frame, interval)) {
                ++staleActorRecords;
                return;
            }
            rejectedParts = job->rejected;
            if (!job->result) { failure = job->failure; return; }
            auto next = std::move(job->result);
            if (GraphicsBackend::NativeDeviceLost() || owner.d3d12Device.get() != backend.d3d12Device.get()) return;
            try {
                next->materials = materialWorker.Update(backend, frame, scope, next->materialSources);
                // Worker 15 retains execution consolidation ownership. This
                // boundary still executes the closed list in queue order.
                next->poseRevision = job->poseRevision;
                next->capturedQpc = job->capturedQpc;
                next->submittedFrame = frame;
                next->readyValue = UINT64_MAX;
                slots[job->slot] = next;
                ID3D12CommandList* lists[]{ next->commands.get() };
                backend.commandQueue->ExecuteCommandLists(1, lists);
                const auto value = ++nextFence;
                Eval::Check(backend.commandQueue->Signal(completion.get(), value));
                next->readyValue = value;
                current = std::move(next);
                failure.clear();
                ++publishedActorRecords;
                const auto age = frame - job->frame;
                if (age) ++approximateActorRecords;
                TracyPlot("DX12::Characters::PublishedPoseAge", int64_t(age));
            } catch (const std::exception& e) {
                failure = e.what();
            }
        }

    public:
        // Called before shadow rendering to overlap owned-pose preparation
        // with rasterization. Final admission still occurs at opaque ready.
        void PrepareActor(const GraphicsBackend& backend, uint32_t frame, uint32_t scope, uint32_t interval)
        {
            ZoneScopedN("DX12::Characters::SnapshotAcquisition");
            if (!scope || (captureFrame == frame && captureScope == scope)) return;
            captureFrame = frame; captureScope = scope;
            PruneActor(frame, scope, interval);
            if (actorJob || !ActorCaptureDue(frame, lastActorCaptureFrame, interval,
                lastActorCaptureScope == scope)) return;
            const auto gate = globals::shaderCache ? globals::shaderCache->GetRuntimeTaskGate() : nullptr;
            if (!gate) { ++deferredActorRecords; return; }
            try {
                Initialize(backend);
                if (completion->GetCompletedValue() == UINT64_MAX) return;
                size_t index = slots.size();
                uint64_t tracked = 0;
                for (size_t i = 0; i < slots.size(); ++i) {
                    auto& slot = slots[i];
                    if (slot) tracked += slot->allocationBytes;
                    if (slot && slot->readyValue == UINT64_MAX) {
                        const auto rescue = ++nextFence;
                        if (SUCCEEDED(backend.commandQueue->Signal(completion.get(), rescue))) slot->readyValue = rescue;
                    }
                    if (index == slots.size() && (!slot || (slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue))))
                        index = i;
                }
                if (index == slots.size()) { ++deferredActorRecords; return; }
                const auto reservation = gate->TryReserve(Util::RuntimePreparationLane::Character);
                if (!reservation) { ++deferredActorRecords; ++actorCaptureAdmissionSkips; return; }
                const SKSE::stl::scope_exit cancelReservation([&] { gate->CompleteReservation(reservation); });
                auto snapshot = std::make_unique<ActorSourceSnapshot>();
                const auto capturedQpc = Util::GetQpcTicks();
                Capture(*snapshot);
                if (snapshot->parts.empty()) {
                    rejectedParts = snapshot->rejected; failure = "No admitted character parts"; return;
                }
                auto job = std::make_shared<ActorRecordJob>();
                job->frame = frame; job->scope = scope; job->interval = ActorCaptureInterval(interval);
                job->poseRevision = ++nextPoseRevision; job->capturedQpc = capturedQpc;
                job->slot = uint32_t(index); job->rejected = snapshot->rejected;
                job->snapshotBytes = snapshot->bytes;
                snapshot->lease.TransferTo(job->lease);
                job->snapshot = std::move(snapshot);
                // Transfer only a retired, exclusively owned slot. On rejection
                // it can be restored because no task was admitted.
                auto selected = std::move(slots[index]);
                std::weak_ptr<ActorRecordJob> weakJob = job;
                if (!gate->CompleteReservation(reservation, [weakJob, selected, tracked,
                    device = backend.d3d12Device, root = root, pipeline = pipeline, completion = completion,
                    frame, scope, interval]() mutable {
                    const auto retained = weakJob.lock();
                    if (!retained) return;
                    try {
                        auto source = std::move(retained->snapshot);
                        auto snapshot = DecodeActor(*source);
                        source.reset();
                        retained->rejected = snapshot.rejected;
                        if (snapshot.vertices.empty() || snapshot.indices.empty())
                            throw std::runtime_error("No admitted character parts");
                        retained->result = RecordActor(snapshot, std::move(selected), tracked, device,
                            root, pipeline, completion, frame, scope, interval);
                    } catch (const std::exception& error) { retained->failure = error.what(); }
                    catch (...) { retained->failure = "Character recording failed"; }
                    retained->finished.store(true, std::memory_order_release);
                })) {
                    slots[index] = std::move(selected);
                    ++deferredActorRecords;
                    return;
                }
                actorJob = std::move(job);
                lastActorCaptureFrame = frame; lastActorCaptureScope = scope;
                ++asyncActorRecords;
            } catch (const std::exception& error) { failure = error.what(); }
        }

    private:

        void PublishMovers(const GraphicsBackend& backend, uint32_t frame, uint32_t scope)
        {
            ZoneScopedN("DX12::RigidMover::Publish");
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
                next->frame = frame; next->scope = scope; next->allowedAge = 0; next->submittedFrame = frame;
                next->poseRevision = 0; next->capturedQpc = 0;
                next->actorSource = current && current->Submitted(frame, scope) ? current : nullptr;
                next->parts = next->actorSource ? next->actorSource->parts : 0;
                next->vertices = next->actorSource ? next->actorSource->vertices : 0;
                next->triangles = next->actorSource ? next->actorSource->triangles : 0;
                next->rigidResources.clear();
                next->rigidBuilds.clear();
                next->rigidInstances = 0; next->rigidShadingCount = 0;
                next->rigidShading = nullptr; next->materials.reset();
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
                std::vector<std::shared_ptr<RigidMesh>> shadingOrder;
                CharacterMaterialWorker::Sources materialSources;
                if (next->actorSource)
                    CharacterMaterialWorker::AppendRows(materialSources, next->actorSource->materialSources);
                std::vector<std::shared_ptr<RigidMesh>> built;
                for (const auto& mover : movers) {
                    auto& mesh = *mover.mesh;
                    auto blas = mesh.blas;
                    if (!blas) {
                        if (!mesh.recorded || !mesh.recorded->blas) continue;
                        const auto pending = std::ranges::find(built, mover.mesh);
                        if (pending == built.end()) {
                            if (built.size() >= kMaxNewMovers) continue;
                            built.push_back(mover.mesh);
                            next->rigidBuilds.push_back(mesh.recorded);
                        }
                        blas = mesh.recorded->blas;
                    }
                    next->rigidResources.push_back(blas);
                    D3D12_RAYTRACING_INSTANCE_DESC instance{};
                    std::memcpy(instance.Transform, mover.transform.data(), sizeof(instance.Transform));
                    instance.InstanceID = UINT(instances.size()); instance.InstanceMask = 0xff;
                    instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
                    instance.AccelerationStructure = blas->GetGPUVirtualAddress();
                    instances.push_back(instance); ++next->rigidInstances;
                    shadingOrder.push_back(mover.mesh);
                    materialSources.push_back(mover.material);
                }
                if (!next->rigidInstances) { Eval::Check(cmd->Close()); return; }
                if (materialSources.size() == next->parts + next->rigidInstances) {
                    // Shading is optional for visibility. A failed allocation or
                    // deferred material generation must not interrupt the rigid TLAS.
                    try {
                        next->rigidShading = PrepareMoverShading(device.get(), shadingOrder);
                        if (next->rigidShading) {
                            next->rigidShadingCount = next->rigidInstances;
                            next->materials = moverMaterialWorker.Update(backend, frame, scope,
                                std::make_shared<const CharacterMaterialWorker::Sources>(std::move(materialSources)));
                        }
                    } catch (const std::exception&) {
                        next->rigidShading = nullptr; next->rigidShadingCount = 0;
                    }
                }
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
                std::vector<ID3D12CommandList*> lists;
                lists.reserve(next->rigidBuilds.size() + 1);
                for (const auto& recorded : next->rigidBuilds) lists.push_back(recorded->commands.get());
                lists.push_back(cmd);
                // Lists contain BLAS UAV barriers. Submit the recorded local
                // meshes before the current-frame instance TLAS.
                backend.commandQueue->ExecuteCommandLists(UINT(lists.size()), lists.data());
                for (const auto& mesh : built) {
                    mesh->blas = mesh->recorded->blas;
                    mesh->recorded.reset();
                }
                const auto value = ++nextFence;
                Eval::Check(backend.commandQueue->Signal(completion.get(), value));
                next->readyValue = value;
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
        using MeshKey = std::array<uintptr_t, 11>;
        struct RigidSourcePacket
        {
            RE::BSGraphics::VertexDesc desc{};
            Bone transform{};
            RE::NiBound bound{};
            std::vector<uint8_t> vertices;
            std::vector<uint16_t> indices;
        };
        struct PackedRigidMesh
        {
            std::vector<uint32_t> shadingIndices;
            std::vector<float> shading;
            std::shared_ptr<const RigidRecordedBuild> recorded;
        };
        struct RigidPackJob
        {
            std::atomic<bool> finished{ false };
            uint64_t snapshotBytes = 0;
            PreparationLease lease{ rigidPreparationBytes };
            std::unique_ptr<RigidSourcePacket> packet;
            // Written only by the worker, read only after acquire of finished.
            std::unique_ptr<PackedRigidMesh> result;
        };
        static void PackRigidMesh(const std::shared_ptr<RigidPackJob>& job,
            const winrt::com_ptr<ID3D12Device>& device12, bool withShading) noexcept
        {
            ZoneScopedN("DX12::RigidMover::PackSnapshot");
            try {
                auto packet = std::move(job->packet);
                if (!packet || !device12) throw std::runtime_error("Missing rigid source");
                auto device = device12.try_as<ID3D12Device5>();
                if (!device) throw std::runtime_error("DXR device unavailable");
                auto result = std::make_unique<PackedRigidMesh>();
                const auto stride = packet->desc.GetSize();
                const auto count = packet->vertices.size() / stride;
                std::vector<std::array<float, 3>> positions;
                positions.reserve(count);
                if (withShading) result->shading.reserve(count * 18u);
                for (size_t i = 0; i < count; ++i) {
                    Vertex v{};
                    const auto* source = packet->vertices.data() + i * stride;
                    std::memcpy(v.position.data(), source, sizeof(v.position));
                    if (!Finite(v.position)) throw std::runtime_error("Invalid rigid vertex");
                    positions.push_back(v.position);
                    if (withShading) {
                        CaptureAttributes(packet->desc, source, v);
                        for (const auto& field : { std::span<const float>(v.position), std::span<const float>(v.normal),
                            std::span<const float>(v.uv), std::span<const float>(v.color),
                            std::span<const float>(v.tangent), std::span<const float>(v.bitangent) })
                            result->shading.insert(result->shading.end(), field.begin(), field.end());
                    }
                }
                const auto step = std::max<size_t>(1, count / 32);
                for (size_t i = 0; i < count; i += step) {
                    const auto& v = positions[i]; const auto& transform = packet->transform;
                    std::array<float, 3> p{};
                    for (uint32_t row = 0; row < 3; ++row)
                        p[row] = transform[row*4]*v[0] + transform[row*4+1]*v[1] + transform[row*4+2]*v[2] + transform[row*4+3];
                    if (!InsideBound(packet->bound, p)) throw std::runtime_error("Invalid rigid bounds");
                }
                std::vector<uint32_t> indices;
                indices.reserve(packet->indices.size());
                for (const auto index : packet->indices) {
                    if (index >= count) throw std::runtime_error("Invalid rigid index");
                    indices.push_back(index);
                }
                if (withShading) result->shadingIndices = indices;
                {
                    ZoneScopedN("DX12::RigidMover::RecordBLAS");
                    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
                    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                    geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(positions.front());
                    geometry.Triangles.VertexCount = UINT(positions.size());
                    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                    geometry.Triangles.IndexCount = UINT(indices.size());
                    geometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
                    input.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                    input.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
                    input.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; input.NumDescs = 1; input.pGeometryDescs = &geometry;
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
                    device->GetRaytracingAccelerationStructurePrebuildInfo(&input, &info);
                    if (!info.ResultDataMaxSizeInBytes || !info.ScratchDataSizeInBytes)
                        throw std::runtime_error("Rigid BLAS unsupported");
                    constexpr auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                    uint64_t required = 0;
                    const auto add = [&](uint64_t bytes, D3D12_RESOURCE_FLAGS flags) {
                        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
                        const auto size = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
                        if (!size || size == UINT64_MAX || size > UINT64_MAX - required)
                            throw std::runtime_error("Rigid BLAS allocation unsupported");
                        required += size;
                    };
                    add(positions.size() * sizeof(positions.front()), D3D12_RESOURCE_FLAG_NONE);
                    add(indices.size() * sizeof(uint32_t), D3D12_RESOURCE_FLAG_NONE);
                    add(info.ResultDataMaxSizeInBytes, uav); add(info.ScratchDataSizeInBytes, uav);
                    VideoMemoryBudget budget;
                    if (required > budget.Query(device.get(), 0).effectiveBytes)
                        throw std::runtime_error("Rigid BLAS budget deferred");
                    auto record = std::make_shared<RigidRecordedBuild>();
                    record->allocationBytes = required;
                    record->positions = Eval::Table(device.get(), positions);
                    record->indices = Eval::Table(device.get(), indices);
                    record->blas = Eval::Buffer(device.get(), info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, uav);
                    record->scratch = Eval::Buffer(device.get(), info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, uav);
                    Eval::Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(record->allocator.put())));
                    Eval::Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, record->allocator.get(), nullptr, IID_PPV_ARGS(record->commands.put())));
                    geometry.Triangles.VertexBuffer.StartAddress = record->positions->GetGPUVirtualAddress();
                    geometry.Triangles.IndexBuffer = record->indices->GetGPUVirtualAddress();
                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
                    build.Inputs = input; build.DestAccelerationStructureData = record->blas->GetGPUVirtualAddress();
                    build.ScratchAccelerationStructureData = record->scratch->GetGPUVirtualAddress();
                    record->commands->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
                    const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(record->blas.get());
                    record->commands->ResourceBarrier(1, &barrier);
                    Eval::Check(record->commands->Close());
                    result->recorded = std::move(record);
                }
                job->result = std::move(result);
            } catch (...) {
                job->result.reset();
            }
            job->finished.store(true, std::memory_order_release);
        }
        struct RigidMesh
        {
            RE::NiPointer<RE::BSGeometry> geometry;
            std::shared_ptr<RigidPackJob> pending;
            uint64_t reservedShadingBytes = 0;
            std::shared_ptr<const RigidRecordedBuild> recorded;
            std::vector<uint32_t> shadingIndices;
            std::vector<float> shading;
            winrt::com_ptr<ID3D12Resource> blas;
            uint32_t lastFrame = 0;
        };
        struct RigidInstance { std::shared_ptr<RigidMesh> mesh; Bone transform; std::shared_ptr<const DX12::PreparedMaterialSource> material; };
        static constexpr uint64_t kMoverShadingBudget = 32ull * VideoMemoryBudget::kMiB;
        static constexpr uint64_t kMoverPendingBudget = 64ull * VideoMemoryBudget::kMiB;
        // Local geometry rows are indexed by the admitted TLAS instance order,
        // excluding the optional actor BLAS. Shared meshes occupy one data range.
        winrt::com_ptr<ID3D12Resource> PrepareMoverShading(ID3D12Device* device,
            const std::vector<std::shared_ptr<RigidMesh>>& order)
        {
            if (moverShadingCache && std::ranges::equal(moverShadingOrder, order,
                [](const auto& previous, const auto& current) { return previous.lock() == current; })) return moverShadingCache;
            if (order.empty() || order.size() > kMaxMovers) return {};
            const size_t tableWords = order.size() * 4u;
            std::vector<uint32_t> words(tableWords);
            std::map<const RigidMesh*, std::array<uint32_t, 4>> ranges;
            bool any = false;
            for (size_t i = 0; i < order.size(); ++i) {
                const auto& mesh = *order[i];
                if (mesh.shading.empty() || mesh.shadingIndices.empty() || mesh.shading.size() % 18u) continue;
                auto it = ranges.find(&mesh);
                if (it == ranges.end()) {
                    const uint64_t count = uint64_t(mesh.shading.size()) + mesh.shadingIndices.size();
                    if ((uint64_t(words.size()) + count) * sizeof(uint32_t) > kMoverShadingBudget) continue;
                    const uint32_t vertexOffset = uint32_t(words.size() * sizeof(uint32_t));
                    const auto start = words.size();
                    words.resize(start + mesh.shading.size());
                    std::memcpy(words.data() + start, mesh.shading.data(), mesh.shading.size() * sizeof(float));
                    const uint32_t indexOffset = uint32_t(words.size() * sizeof(uint32_t));
                    words.insert(words.end(), mesh.shadingIndices.begin(), mesh.shadingIndices.end());
                    it = ranges.emplace(&mesh, std::array{ vertexOffset, indexOffset,
                        uint32_t(mesh.shading.size() / 18u), uint32_t(mesh.shadingIndices.size()) }).first;
                }
                std::copy(it->second.begin(), it->second.end(), words.begin() + i * 4u);
                any = true;
            }
            if (!any) return {};
            const auto desc = CD3DX12_RESOURCE_DESC::Buffer(uint64_t(words.size()) * sizeof(uint32_t));
            const uint64_t allocation = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
            if (!allocation || allocation == UINT64_MAX || allocation > budget.Query(device, 0).effectiveBytes) return {};
            auto resource = Eval::Table(device, words);
            moverShadingOrder.clear(); moverShadingOrder.reserve(order.size());
            for (const auto& mesh : order) moverShadingOrder.emplace_back(mesh);
            moverShadingCache = resource;
            return resource;
        }
        winrt::com_ptr<ID3D12Resource> moverShadingCache;
        std::vector<std::weak_ptr<RigidMesh>> moverShadingOrder;
        std::map<MeshKey, std::shared_ptr<RigidMesh>> moverMeshes;
        std::vector<RigidInstance> movers;
        std::array<std::shared_ptr<CharacterCasterFrame>, 4> moverSlots;
        std::shared_ptr<CharacterCasterFrame> combined;
        uint32_t moverScope = 0, newMoverMeshes = 0;
        uint64_t moverGeometryRevision = 0;
        uint64_t asyncRigidPacks = 0, inlineRigidPacks = 0, deferredRigidPacks = 0;
        GraphicsBackend owner;
        std::shared_ptr<ActorRecordJob> actorJob;
        uint32_t captureFrame = UINT32_MAX, captureScope = 0;
        uint32_t lastActorCaptureFrame = UINT32_MAX, lastActorCaptureScope = 0;
        uint64_t nextPoseRevision = 0, cancelledActorRecords = 0, approximateActorRecords = 0;
        uint64_t actorCaptureAdmissionSkips = 0, rigidCaptureAdmissionSkips = 0;
        uint64_t asyncActorRecords = 0, deferredActorRecords = 0, staleActorRecords = 0, publishedActorRecords = 0;
        CharacterMaterialWorker materialWorker, moverMaterialWorker;
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

#pragma once

#include "DX12GpuRetirement.h"
#include "GraphicsBackend.h"
#include "Globals.h"
#include "RE/B/BSLightingShaderProperty.h"
#include "DX12NativeMaterialEvaluation.h"
#include "Utils/ActorUtils.h"
#include "Utils/FileSystem.h"
#include "Utils/SkinnedMesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace DX12Native
{
    // A frame-local world-space caster-only scene. No material IDs are shared
    // with the static TLAS, and this scene cannot supply primary receivers.
    struct CharacterCasterFrame
    {
        uint32_t frame = UINT32_MAX, scope = 0, parts = 0, vertices = 0, triangles = 0;
        uint64_t allocationBytes = 0, readyValue = 0;
        winrt::com_ptr<ID3D12Fence> readyFence;
        winrt::com_ptr<ID3D12Resource> acceleration, bottom, positions, scratch, bind, palette, indices, instance;
        winrt::com_ptr<ID3D12CommandAllocator> allocator;
        winrt::com_ptr<ID3D12GraphicsCommandList4> commands;
        winrt::com_ptr<ID3D12RootSignature> root;
        winrt::com_ptr<ID3D12PipelineState> pipeline;

        bool Submitted(uint32_t expectedFrame, uint32_t expectedScope) const
        {
            return frame == expectedFrame && scope == expectedScope && acceleration && readyFence &&
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
        };
        using Bone = std::array<float, 12>;
        static_assert(sizeof(Vertex) == 44 && sizeof(Bone) == 48);

        struct Snapshot
        {
            std::vector<Vertex> vertices;
            std::vector<Bone> bones;
            std::vector<uint32_t> indices;
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
            ++out.parts;
            return true;
        }

        Snapshot Capture(uint32_t frame)
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
            // Rigid non-actor movers are captured by the ordinary draw hook.
            // Their CPU data is copied at observation time, never borrowed across frames.
            if (moverFrame == frame) {
                const auto vertexBase = uint32_t(result.vertices.size());
                const auto boneBase = uint32_t(result.bones.size());
                if (result.parts + observedMovers.parts <= kMaxParts &&
                    result.vertices.size() + observedMovers.vertices.size() <= kMaxVertices &&
                    result.indices.size() + observedMovers.indices.size() <= kMaxIndices &&
                    result.bones.size() + observedMovers.bones.size() <= kMaxBones) {
                    for (auto v : observedMovers.vertices) {
                        for (auto& bone : v.bones) bone += boneBase;
                        result.vertices.push_back(v);
                    }
                    result.bones.insert(result.bones.end(), observedMovers.bones.begin(), observedMovers.bones.end());
                    for (auto index : observedMovers.indices) result.indices.push_back(vertexBase + index);
                    result.parts += observedMovers.parts;
                }
                result.rejected += observedMovers.rejected;
            }
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
            current.reset();
            for (auto& slot : slots)
                if (slot && !GpuFenceRetired(slot->readyFence.get(), slot->readyValue)) {
                    const auto value = slot->readyValue;
                    RetireAfterFences(std::move(slot), owner.d3d12Device, { FencePoint(completion.get(), value) });
                }
        }

        void ObserveRigidMover(RE::BSGeometry& geometry, uint32_t frame)
        {
            if (moverFrame != frame) {
                observedMovers = {};
                seenMovers.clear();
                moverFrame = frame;
            }
            if (observedMovers.parts >= kMaxParts || seenMovers.size() >= kMaxParts ||
                std::ranges::find(seenMovers, &geometry) != seenMovers.end() ||
                geometry.GetGeometryRuntimeData().skinInstance)
                return;
            seenMovers.push_back(&geometry);
            if (!CapturePart(&geometry, observedMovers)) ++observedMovers.rejected;
        }

        void Reset()
        {
            current.reset();
            attemptedFrame = UINT32_MAX; attemptedScope = 0;
            moverFrame = UINT32_MAX; observedMovers = {}; seenMovers.clear();
        }
        std::shared_ptr<const CharacterCasterFrame> Get(uint32_t frame, uint32_t scope) const
        {
            return current && current->Submitted(frame, scope) ? current : nullptr;
        }
        const std::string& Failure() const { return failure; }
        uint32_t RejectedParts() const { return rejectedParts; }

        void Publish(const GraphicsBackend& backend, uint32_t frame, uint32_t scope)
        {
            if (!scope || (attemptedFrame == frame && attemptedScope == scope)) return;
            attemptedFrame = frame; attemptedScope = scope; current.reset();
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
                auto snapshot = Capture(frame);
                rejectedParts = snapshot.rejected;
                if (snapshot.vertices.empty() || snapshot.indices.empty()) { failure = "No admitted character parts"; return; }
                auto device = backend.d3d12Device.try_as<ID3D12Device5>();
                if (!device) throw std::runtime_error("DXR device unavailable");
                D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
                geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                geometry.Triangles.VertexCount = UINT(snapshot.vertices.size());
                geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                geometry.Triangles.VertexBuffer.StrideInBytes = 12;
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
                const uint64_t positionBytes = snapshot.vertices.size() * 12ull;
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
                next->frame = frame; next->scope = scope; next->parts = snapshot.parts;
                next->vertices = uint32_t(snapshot.vertices.size()); next->triangles = uint32_t(snapshot.indices.size()/3);
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

    private:
        GraphicsBackend owner;
        VideoMemoryBudget budget;
        std::array<std::shared_ptr<CharacterCasterFrame>, 3> slots;
        std::shared_ptr<CharacterCasterFrame> current;
        winrt::com_ptr<ID3D12RootSignature> root;
        winrt::com_ptr<ID3D12PipelineState> pipeline;
        winrt::com_ptr<ID3D12Fence> completion;
        uint64_t nextFence = 0;
        uint32_t attemptedFrame = UINT32_MAX, attemptedScope = 0, rejectedParts = 0;
        uint32_t moverFrame = UINT32_MAX;
        Snapshot observedMovers;
        std::vector<const RE::BSGeometry*> seenMovers;
        std::string failure;
    };
}

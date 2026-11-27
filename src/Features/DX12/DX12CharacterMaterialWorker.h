#pragma once

#include "DX12NativeMaterialEvaluation.h"
#include "DX12GpuRetirement.h"
#include "Features/NativePBR/MaterialCatalog.h"
#include "Features/NativePBR/NativeMaterialMetadata.h"
#include "GraphicsBackend.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "RE/B/BSLightingShaderProperty.h"
#include "RE/B/BSTextureSet.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <memory>
#include <vector>

namespace DX12Native
{
    // Engine-pointer-free material metadata, one row per admitted actor part.
    // The worker never reads a live shader property, texture set or geometry.
    inline DX12::PreparedMaterialSource CaptureCharacterMaterial(RE::BSGeometry& geometry, bool hasColor)
    {
        DX12::PreparedMaterialSource source;
        const auto& runtime = geometry.GetGeometryRuntimeData();
        const auto* baseProperty = runtime.shaderProperty.get();
        if (!baseProperty || baseProperty->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get() || runtime.alphaProperty)
            return source;
        const auto* property = static_cast<const RE::BSLightingShaderProperty*>(baseProperty);
        if (!property->material || property->material->GetType() != RE::BSShaderMaterial::Type::kLighting ||
            property->material->GetFeature() != RE::BSShaderMaterial::Feature::kDefault)
            return source;
        source.authoredShaderFlags = NativePBR::ReadAuthoredFlags(*property);
        if (!source.authoredShaderFlags || (!hasColor && (*source.authoredShaderFlags & (uint64_t(1) << 37)))) return {};
        const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
        auto* textures = material->textureSet.get();
        const auto* expected = REL::Relocation<const RE::NiRTTI*>{ RE::BSTextureSet::Ni_RTTI }.get();
        if (!textures || !expected || !textures->GetRTTI() || !textures->GetRTTI()->IsKindOf(expected)) return {};
        source.observedShaderFlags = static_cast<uint64_t>(property->flags.underlying());
        source.textureSetEditorId = NativePBR::ResolveTextureSet(textures);
        NativePBR::ReadNifParameters(*property, source.nifThinSurface, source.nifFuzz);
        source.alpha = material->materialAlpha;
        source.clampMode = material->textureClampMode;
        source.uvTransform = { material->texCoordOffset[0].x, material->texCoordOffset[0].y,
            material->texCoordScale[0].x, material->texCoordScale[0].y };
        if (!std::ranges::all_of(source.uvTransform, [](float v) { return std::isfinite(v); }) ||
            !std::isfinite(source.alpha)) return {};
        source.colorOffset = hasColor ? 0u : UINT32_MAX;
        for (uint32_t i = 0; i < source.texturePaths.size(); ++i) {
            const char* path = textures->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i));
            if (path) {
                const std::string_view value(path);
                if (value.size() > 260) return {};
                source.texturePaths[i] = value;
            }
        }
        return source;
    }

    struct CharacterMaterialPublication
    {
        std::shared_ptr<const NativeMaterialEvaluation> evaluation;
        winrt::com_ptr<ID3D12Fence> readyFence;
        uint64_t generation = 0;
        uint32_t scope = 0;
        bool Ready() const
        {
            if (!evaluation) return false;
            const uint64_t completed = readyFence ? readyFence->GetCompletedValue() : 1u;
            return completed != UINT64_MAX && completed >= 1u;
        }
    };

    /** @brief Latest-only residency coordinator using the shared runtime CPU task gate.
     * Requests and results are immutable, scoped and revision checked. No CPU GPU wait.
     */
    class CharacterMaterialWorker
    {
        using Eval = NativeMaterialEvaluation;
        using Sources = std::vector<DX12::PreparedMaterialSource>;
        struct State
        {
            std::mutex mutex;
            GraphicsBackend backend;
            std::shared_ptr<const Sources> sources;
            std::shared_ptr<const NativePBR::MaterialCatalogSnapshot> catalog;
            std::shared_ptr<const CharacterMaterialPublication> last;
            uint64_t generation = 0, attempted = 0;
            uint32_t scope = 0, retryFrame = 0;
            bool scheduled = false, closing = false;
        };
        struct UploadBatch
        {
            std::shared_ptr<const CharacterMaterialPublication> publication;
            std::vector<Eval::Upload> uploads;
            winrt::com_ptr<ID3D12CommandAllocator> allocator;
            winrt::com_ptr<ID3D12GraphicsCommandList> commands;
        };

        static void Run(const std::shared_ptr<State>& state, uint32_t frame)
        {
            std::shared_ptr<const Sources> sources;
            std::shared_ptr<const NativePBR::MaterialCatalogSnapshot> catalog;
            std::shared_ptr<const CharacterMaterialPublication> previous;
            GraphicsBackend backend;
            uint64_t generation = 0;
            uint32_t scope = 0;
            {
                std::scoped_lock lock(state->mutex);
                if (state->closing || !state->sources) { state->scheduled = false; return; }
                sources = state->sources; catalog = state->catalog; previous = state->last;
                backend = state->backend; generation = state->generation; scope = state->scope;
            }
            std::shared_ptr<CharacterMaterialPublication> result;
            bool retry = false;
            try {
                PreparedMaterialFrame prepared;
                prepared.geometryRevision = generation; prepared.scope = scope;
                prepared.bindings.resize(sources->size());
                std::vector<Eval::BindingSource> bindings(sources->size());
                for (size_t i = 0; i < sources->size(); ++i) {
                    const auto& source = (*sources)[i];
                    // Ordinal identities are local to this independent material generation.
                    auto recipe = CompilePreparedMeshRecipe(source, i + 1u, generation, catalog.get());
                    if (!recipe) continue;
                    bindings[i] = { *source.authoredShaderFlags, source.observedShaderFlags, true,
                        source.colorOffset != UINT32_MAX, false };
                    prepared.bindings[i] = NativePBR::PackGpuDrawBinding(recipe->identity, recipe->revision, UINT32_MAX, 0);
                    prepared.recipes.push_back(std::move(*recipe));
                }
                if (!prepared.recipes.empty()) {
                    auto batch = std::make_shared<UploadBatch>();
                    const Eval::TextureCache empty;
                    const auto& cache = previous && previous->scope == scope && previous->Ready() ? previous->evaluation->textures : empty;
                    auto evaluation = Eval::PrepareForBindings(backend.d3d12Device.get(), prepared, bindings, cache, batch->uploads);
                    if (!evaluation) retry = true;
                    else {
                        result = std::make_shared<CharacterMaterialPublication>();
                        // Retry transient budget/allocation deferrals without discarding a usable partial publication.
                        retry = evaluation->failureReasons.contains("DXGI upload budget") ||
                            evaluation->failureReasons.contains("source budget") ||
                            evaluation->failureReasons.contains("GPU allocation/upload setup");
                        result->generation = generation; result->scope = scope; result->evaluation = std::move(evaluation);
                        if (!batch->uploads.empty()) {
                            auto* device = backend.d3d12Device.get();
                            Eval::Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(result->readyFence.put())));
                            Eval::Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(batch->allocator.put())));
                            Eval::Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, batch->allocator.get(), nullptr, IID_PPV_ARGS(batch->commands.put())));
                            Eval::RecordUploads(batch->commands.get(), batch->uploads);
                            Eval::Check(batch->commands->Close());
                            batch->publication = result;
                            ID3D12CommandList* lists[]{ batch->commands.get() };
                            backend.commandQueue->ExecuteCommandLists(1, lists);
                            const auto signal = backend.commandQueue->Signal(result->readyFence.get(), 1u);
                            RetireAfterFences(std::move(batch), backend.d3d12Device,
                                { FencePoint(result->readyFence.get(), SUCCEEDED(signal) ? 1u : UINT64_MAX) });
                            Eval::Check(signal);
                        }
                    }
                }
            } catch (const std::exception& error) {
                result.reset(); retry = true;
                logger::warn("[DX12] Character material residency: {}", error.what());
            } catch (...) {
                result.reset(); retry = true;
                logger::warn("[DX12] Character material residency failed");
            }
            std::scoped_lock lock(state->mutex);
            state->scheduled = false;
            if (!state->closing && state->generation == generation) {
                state->attempted = generation;
                state->retryFrame = retry ? frame + 120u : 0u;
                if (result) state->last = std::move(result);
                else if (!retry) state->last.reset();
            }
        }

    public:
        ~CharacterMaterialWorker()
        {
            if (!state) return;
            std::scoped_lock lock(state->mutex);
            state->closing = true; state->sources.reset(); state->last.reset(); ++state->generation;
        }
        void Reset()
        {
            if (!state) return;
            std::scoped_lock lock(state->mutex);
            state->sources.reset(); state->last.reset(); state->catalog.reset();
            state->scope = 0; state->retryFrame = 0; ++state->generation;
        }
        std::shared_ptr<const CharacterMaterialPublication> Update(const GraphicsBackend& backend,
            uint32_t frame, uint32_t scope, Sources sources)
        {
            if (!state) state = std::make_shared<State>();
            const auto catalog = NativePBR::MaterialCatalog::Shared().GetSnapshot();
            std::scoped_lock lock(state->mutex);
            if (state->scope != scope || state->backend.d3d12Device.get() != backend.d3d12Device.get() ||
                state->backend.commandQueue.get() != backend.commandQueue.get()) {
                state->last.reset();
                state->sources.reset();
            }
            if (!state->sources || *state->sources != sources || state->catalog != catalog || state->scope != scope) {
                state->sources = std::make_shared<const Sources>(std::move(sources));
                state->catalog = catalog; state->scope = scope; state->backend = backend;
                ++state->generation; state->retryFrame = 0;
            }
            const bool retry = state->retryFrame && int32_t(frame - state->retryFrame) >= 0;
            const bool uploadPending = state->last && state->last->generation == state->generation && !state->last->Ready();
            if (!state->scheduled && !uploadPending && (state->attempted != state->generation || retry) && globals::shaderCache) {
                if (const auto gate = globals::shaderCache->GetRuntimeTaskGate()) {
                    state->scheduled = true;
                    if (!gate->TrySubmit([retained = state, frame] { Run(retained, frame); }))
                        state->scheduled = false;
                }
            }
            return state->last && state->last->generation == state->generation &&
                state->last->scope == scope && state->last->Ready() ? state->last : nullptr;
        }
    private:
        std::shared_ptr<State> state;
    };
}

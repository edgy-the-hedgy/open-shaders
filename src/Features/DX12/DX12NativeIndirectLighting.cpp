#include "DX12NativeIndirectLighting.h"
#include "Features/NativeIBL/NativeIBLPolicy.h"
#include "DX12GpuRetirement.h"
#include "DX12FullNativeRasterEvidence.h"
#include "GpuPass.h"
#include "Utils/FileSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <directx/d3dx12.h>
#include <set>
#include <stdexcept>
#include <vector>

namespace DX12Native
{
    namespace
    {
        using Eval = NativeMaterialEvaluation;
        using namespace MaterialFrameContract;
        struct Constants
        {
            Matrix projectionInverse, viewInverse;
            uint32_t width, height, materialPitch, lightingPitch;
            uint32_t enabled, available, environmentMips, reflectionMips;
            float environmentScale, skyScale;
            uint32_t interior, colorEncoding;
            NativeAmbientParameters ambient;
            float4 positionAdjust;
            uint32_t instanceCount, characterCastersEnabled = 0, quality = kShadowQualityQuality, frame = 0;
            Matrix projection, view;
            uint32_t ssrEnabled, directPitch, rayPitch, tokenCount;
            float4 secondarySunDirection, secondarySunRadiance;
            uint32_t secondaryLightCount, secondaryFoliage, skyAvailable, secondaryMaterialControls;
            uint32_t secondaryProjectionRows, characterVertices, characterTriangles, characterParts;
            uint32_t characterMovers, materialLobePitch, characterPadding[2]{};
        };
        static_assert(sizeof(Constants) == 544);
        constexpr uint64_t kSHBytes = 48;
        constexpr UINT kCharacterTextureOffset = 6 + NativePBR::kGpuTextureDescriptorCapacity + kProjectedTextureCapacity;
        constexpr UINT kSceneDescriptor = kCharacterTextureOffset + NativePBR::kGpuTextureDescriptorCapacity;
        constexpr UINT kCharacterSceneDescriptor = kSceneDescriptor + 1u;

        void BuildPipeline(ID3D12Device* device, const std::filesystem::path& path, UINT count, CD3DX12_ROOT_PARAMETER* parameters,
            winrt::com_ptr<ID3D12RootSignature>& root, winrt::com_ptr<ID3D12PipelineState>& pipeline)
        {
            const auto shader = Util::FileHelpers::ReadBoundedBinary(path, 16 * VideoMemoryBudget::kMiB);
            std::array<CD3DX12_STATIC_SAMPLER_DESC, 4> samplers;
            for (uint32_t mode = 0; mode < samplers.size(); ++mode)
                samplers[mode] = CD3DX12_STATIC_SAMPLER_DESC(mode, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                    (mode & 2) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                    (mode & 1) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                    D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
            const CD3DX12_ROOT_SIGNATURE_DESC signature(count, parameters, UINT(samplers.size()), samplers.data());
            winrt::com_ptr<ID3DBlob> blob, errors;
            Eval::Check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put()));
            Eval::Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(root.put())));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = root.get();
            pso.CS = { shader.data(), shader.size() };
            Eval::Check(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(pipeline.put())));
        }

        void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
            cmd->ResourceBarrier(1, &barrier);
        }

        void CubeView(ID3D12Device* device, const EnvironmentCube* cube, D3D12_CPU_DESCRIPTOR_HANDLE handle)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = cube ? cube->format : DXGI_FORMAT_R16G16B16A16_FLOAT;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.TextureCube.MipLevels = cube ? cube->mipCount : 1u;
            device->CreateShaderResourceView(cube ? cube->resource.get() : nullptr, &view, handle);
        }
    }

    NativeIndirectLighting::~NativeIndirectLighting()
    {
        current.reset();
        baseCache.reset();
        skyCache.reset();
        for (auto& slot : slots) {
            if (!slot || GpuFenceRetired(slot->readyFence.get(), slot->readyValue))
                continue;
            std::vector<FenceDependency> dependencies{ FencePoint(slot->readyFence.get(), slot->readyValue) };
            RetireAfterFences(std::move(slot), owner.d3d12Device, std::move(dependencies));
        }
    }

    uint64_t NativeIndirectLighting::AllocationBytes() const
    {
        uint64_t bytes = 0;
        std::set<const EnvironmentIrradiance*> seen;
        const auto count = [&](const auto& sh) {
            if (sh && seen.insert(sh.get()).second) bytes += sh->allocationBytes;
        };
        count(baseCache); count(skyCache);
        for (const auto& slot : slots)
            if (slot) { bytes += slot->allocationBytes; count(slot->baseSH); count(slot->skySH); }
        return bytes;
    }

    void NativeIndirectLighting::Poll(uint32_t frame, bool release)
    {
        if (current && (current->frame != frame || release)) current.reset();
        if (release) { baseCache.reset(); skyCache.reset(); }
        for (auto& slot : slots) {
            if (!slot) continue;
            if (slot->readyValue == UINT64_MAX && completion && owner.commandQueue &&
                completion->GetCompletedValue() != UINT64_MAX) {
                const uint64_t rescue = nextFence + 1;
                if (SUCCEEDED(owner.commandQueue->Signal(completion.get(), rescue))) {
                    nextFence = rescue; slot->readyValue = rescue;
                }
            }
            if (slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue)) {
                if (release) { slot.reset(); continue; }
                slot->material.reset(); slot->environment.reset(); slot->characters.reset(); slot->ssrDirect.reset();
                slot->baseSH.reset(); slot->skySH.reset();
            }
        }
    }

    void NativeIndirectLighting::Reset()
    {
        // Drop history and SH leases, but keep retired frame allocations reusable.
        Poll(UINT32_MAX);
        current.reset();
        baseCache.reset();
        skyCache.reset();
        failure.clear();
    }

    std::shared_ptr<const IndirectLightingFrame> NativeIndirectLighting::Get(uint32_t frame) const
    {
        return current && current->frame == frame && current->readyFence &&
            ValidProducerSignal(current->readyValue, current->readyFence->GetCompletedValue()) ? current : nullptr;
    }

    void NativeIndirectLighting::Initialize(const GraphicsBackend& backend)
    {
        if (pipeline) {
            if (owner.d3d12Device.get() != backend.d3d12Device.get() || owner.commandQueue.get() != backend.commandQueue.get())
                throw std::runtime_error("Native indirect device changed; restart required");
            return;
        }
        auto* device = backend.d3d12Device.get();
        CD3DX12_DESCRIPTOR_RANGE projectionRange;
        projectionRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        CD3DX12_ROOT_PARAMETER projectionParameters[3];
        projectionParameters[0].InitAsDescriptorTable(1, &projectionRange);
        projectionParameters[1].InitAsUnorderedAccessView(0);
        projectionParameters[2].InitAsConstants(1, 0);
        CD3DX12_DESCRIPTOR_RANGE ranges[6];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 4, 0, 1);
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, NativePBR::kGpuTextureDescriptorCapacity, 10, 0, 4);
        ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 527, 0, 4 + NativePBR::kGpuTextureDescriptorCapacity);
        ranges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1+kProjectedTextureCapacity,530,0,5+NativePBR::kGpuTextureDescriptorCapacity);
        ranges[5].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,NativePBR::kGpuTextureDescriptorCapacity,4,1,kCharacterTextureOffset);
        CD3DX12_ROOT_PARAMETER parameters[24];
        parameters[0].InitAsShaderResourceView(0);
        parameters[1].InitAsDescriptorTable(6, ranges);
        parameters[2].InitAsShaderResourceView(2);
        parameters[3].InitAsShaderResourceView(3);
        parameters[4].InitAsUnorderedAccessView(0);
        parameters[5].InitAsConstantBufferView(0);
        CD3DX12_DESCRIPTOR_RANGE sceneRange, characterSceneRange;
        sceneRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,6);
        characterSceneRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,523);
        parameters[6].InitAsDescriptorTable(1,&sceneRange);
        parameters[7].InitAsShaderResourceView(7);
        parameters[8].InitAsShaderResourceView(8);
        parameters[9].InitAsShaderResourceView(9);
        parameters[10].InitAsShaderResourceView(522);
        parameters[11].InitAsDescriptorTable(1,&characterSceneRange);
        parameters[12].InitAsShaderResourceView(524);
        parameters[13].InitAsShaderResourceView(525);
        parameters[14].InitAsShaderResourceView(526);
        parameters[15].InitAsShaderResourceView(528);
        parameters[16].InitAsShaderResourceView(529);
        for (UINT i=0;i<4;++i) parameters[17+i].InitAsShaderResourceView(i,1);
        parameters[21].InitAsShaderResourceView(516,1);
        parameters[22].InitAsShaderResourceView(0,2);
        parameters[23].InitAsConstants(2,0,2);
        winrt::com_ptr<ID3D12RootSignature> pr, ir;
        winrt::com_ptr<ID3D12PipelineState> pp, ip;
        winrt::com_ptr<ID3D12Fence> fence;
        BuildPipeline(device, Util::PathHelpers::GetShadersPath() / "NativeComposite" / "NativeDiffuseSH.dxil", 3, projectionParameters, pr, pp);
        BuildPipeline(device, Util::PathHelpers::GetShadersPath() / "NativeComposite" / "NativeIndirectLightingV2.dxil", 24, parameters, ir, ip);
        Eval::Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())));
        owner = backend;
        projectionRoot = std::move(pr); projectionPipeline = std::move(pp);
        root = std::move(ir); pipeline = std::move(ip); completion = std::move(fence);
    }

    void NativeIndirectLighting::Dispatch(const GraphicsBackend& backend,
        std::shared_ptr<const NativeScreenProbe::MaterialFrame> material,
        std::shared_ptr<const EnvironmentFrame> environment, uint32_t enabled, float environmentScale, float skyScale, const NativeAmbientParameters& ambient, std::shared_ptr<const CharacterCasterFrame> characters, uint32_t quality,
        std::shared_ptr<const DirectLightingFrame> ssrDirect)
    {
        CS_GPU_PASS("NativeLighting::Indirect");
        if (!material || !environment || !enabled) return;
        if (current && current->material == material) return;
        Poll(material->frame);
        if (!material->IsCoherent(material->frame, material->width, material->height) ||
            material->scope != environment->stamp.scope ||
            !std::isfinite(environmentScale) || !std::isfinite(skyScale)) {
            failure = "Incoherent indirect inputs"; return;
        }
        const auto& envStamp = environment->stamp;
        if (!environment->base || !environment->base->IsReadyFor(envStamp) ||
            !environment->producerFence || !environment->producerValue ||
            environment->producerFence->GetCompletedValue() == UINT64_MAX ||
            environment->producerFence->GetCompletedValue() < environment->producerValue ||
            (environment->reflection && !environment->reflection->IsReadyFor(envStamp)) ||
            (environment->sky && !environment->sky->IsReadyFor(envStamp))) {
            failure = "Waiting for completed native environment"; return;
        }
        if (!ambient.iblEnabled || ((ambient.dalcMode >= 2 || ambient.dalcAmount != 0) && !ambient.dalcValid)) {
            failure = "Incoherent native irradiance policy"; return;
        }
        uint32_t available = enabled & (uint32_t(IndirectDiffuse) | uint32_t(IndirectSpecular));
        if ((enabled & Transmission) && (available & IndirectDiffuse)) available |= Transmission;
        if (!available) { failure = "Waiting for completed environment inputs"; return; }
        const uint32_t width = material->width, height = material->height;
        const RasterIdentityStamp stamp{material->frame, material->scope, material->geometryRevision, material->materialRevision};
        if (!ssrDirect || ssrDirect->material != material || ssrDirect->frame != material->frame ||
            ssrDirect->quality != SanitizeRTQuality(quality) || ssrDirect->width != width || ssrDirect->height != height ||
            !ssrDirect->lights || ssrDirect->lightCount > DX12::kMaxPreparedLights ||
            !ssrDirect->samples || !ValidLayout(width,height,ssrDirect->rowPitch,kLightingStride) ||
            ssrDirect->samples->GetDesc().Width < uint64_t(ssrDirect->rowPitch)*height ||
            !ssrDirect->readyFence || !ValidProducerSignal(ssrDirect->readyValue,ssrDirect->readyFence->GetCompletedValue()))
            ssrDirect.reset();
        // Integer raw inputs cannot supply retired 16-bit token evidence.
        // Inline ray/secondary shading and the environment miss remain active.
        const uint64_t bytes = uint64_t(width) * height * kLightingStride;
        if (!bytes || bytes > UINT32_MAX || width > UINT32_MAX / kLightingStride) return;
        std::shared_ptr<IndirectLightingFrame>* selected = nullptr;
        bool submitted = false;
        try {
            Initialize(backend);
            if (completion->GetCompletedValue() == UINT64_MAX) throw std::runtime_error("Indirect device removed");
            for (auto& slot : slots)
                if (!slot || (slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue))) {
                    selected = &slot;
                    if (slot && slot->width == width && slot->height == height) break;
                }
            if (!selected) { failure = "Indirect frame pool busy"; return; }
            auto& slot = *selected;
            auto* device = backend.d3d12Device.get();
            const bool fresh = !slot || slot->width != width || slot->height != height;
            const auto allocation = [&](uint64_t size) {
                const auto desc = CD3DX12_RESOURCE_DESC::Buffer(size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                return device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
            };
            const bool needsBaseSH = (available & (IndirectDiffuse | IndirectSpecular)) != 0;
            const bool needsSkySH = ambient.iblEnabled && (available & IndirectDiffuse) && !environment->stamp.interior &&
                !environment->stamp.skyHidden && bool(environment->sky);
            // A temporarily absent sky is not consumed. Retain its cached lease until
            // a different source arrives or an environment epoch is invalidated.
            const bool projectBase = needsBaseSH && (!baseCache || baseCache->cube != environment->base);
            const bool projectSky = needsSkySH &&
                (!skyCache || skyCache->cube != environment->sky);
            const uint64_t outputAllocation = fresh ? allocation(bytes) : 0;
            const uint64_t shAllocation = (projectBase || projectSky) ? allocation(kSHBytes) : 0;
            if (outputAllocation == UINT64_MAX || shAllocation == UINT64_MAX) throw std::runtime_error("Indirect allocation unsupported");
            const uint64_t required = outputAllocation + (fresh ? 1024 * 1024 : 0) +
                shAllocation * (uint64_t(projectBase) + uint64_t(projectSky));
            if (required) {
                const uint64_t tracked = AllocationBytes();
                const auto allowance = budget.Query(device, tracked);
                if (!allowance.localBudget || required > allowance.effectiveBytes || tracked > allowance.effectiveBytes - required)
                    throw std::runtime_error("Indirect lighting deferred by DXGI budget");
            }
            if (fresh) {
                auto replacement = std::make_shared<IndirectLightingFrame>();
                replacement->width = width; replacement->height = height; replacement->rowPitch = width * kLightingStride;
                replacement->allocationBytes = outputAllocation + 1024 * 1024;
                replacement->samples = Eval::Buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                replacement->samples->SetName(L"NativeLighting::IndirectDiffuseSpecular");
                replacement->constants = Eval::Buffer(device, 768, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                D3D12_DESCRIPTOR_HEAP_DESC heap{};
                heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = kCharacterSceneDescriptor + 1u; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                Eval::Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(replacement->descriptors.put())));
                Eval::Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(replacement->allocator.put())));
                Eval::Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, replacement->allocator.get(), pipeline.get(), IID_PPV_ARGS(replacement->commands.put())));
                slot = std::move(replacement);
            } else {
                Eval::Check(slot->allocator->Reset());
                Eval::Check(slot->commands->Reset(slot->allocator.get(), pipeline.get()));
            }
            slot->material = std::move(material); slot->environment = std::move(environment);
            slot->ssrDirect = std::move(ssrDirect);
            const bool hasSecondary = slot->material->HasSecondaryGeometry();
            slot->screenSpaceReflections = hasSecondary && bool(slot->ssrDirect) && (available & IndirectSpecular);
            if (!hasSecondary) slot->ssrDirect.reset();
            slot->frame = slot->material->frame; slot->quality = SanitizeRTQuality(quality); slot->available = available;
            slot->characters = hasSecondary && characters && characters->Submitted(slot->frame, slot->material->scope) ? std::move(characters) : nullptr;
            slot->skyVisibilityTraced = false;
            slot->root = root; slot->pipeline = pipeline;
            slot->projectionRoot = projectionRoot; slot->projectionPipeline = projectionPipeline;
            slot->readyFence = completion; slot->readyValue = UINT64_MAX;
            const auto& env = *slot->environment;
            // Stable content key for Worker 09: optional sources can disappear
            // without changing the producer\'s base-generation revision.
            slot->environmentContentRevision = NativeIBL::Policy::EnvironmentContentKey(
                env.stamp.epoch, env.revision, uint32_t(env.stamp.encoding), env.base->revision,
                env.reflection ? env.reflection->revision : 0u, env.sky ? env.sky->revision : 0u);
            const auto& raster = *slot->material->raster;
            auto* cmd = slot->commands.get();
            const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            const auto cpu = [&](UINT i) { return CD3DX12_CPU_DESCRIPTOR_HANDLE(slot->descriptors->GetCPUDescriptorHandleForHeapStart(), INT(i), increment); };
            const auto gpu = [&](UINT i) { return CD3DX12_GPU_DESCRIPTOR_HANDLE(slot->descriptors->GetGPUDescriptorHandleForHeapStart(), INT(i), increment); };
            D3D12_SHADER_RESOURCE_VIEW_DESC depthView{};
            depthView.Format = DXGI_FORMAT_R32_FLOAT; depthView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            depthView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; depthView.Texture2D.MipLevels = 1;
            auto* depth = raster.depth->resource.get();
            device->CreateShaderResourceView(depth, &depthView, cpu(0));
            CubeView(device, env.base.get(), cpu(1));
            CubeView(device, env.reflection ? env.reflection.get() : env.base.get(), cpu(2));
            CubeView(device, env.sky ? env.sky.get() : env.base.get(), cpu(3));
            const auto* scene = hasSecondary ? slot->material->scene.get() : nullptr;
            const auto& sceneEvaluation = scene ? *scene->materialEvaluation : *slot->material->rasterMaterialEvaluation;
            sceneEvaluation.WriteTextureViews(device, cpu(4));
            // Environment-only frames bind legal null acceleration descriptors.
            // No ray instruction is executed in that mode.
            D3D12_SHADER_RESOURCE_VIEW_DESC accelerationView{};
            accelerationView.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
            accelerationView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            accelerationView.RaytracingAccelerationStructure.Location=scene ? scene->acceleration->GetGPUVirtualAddress() : 0u;
            device->CreateShaderResourceView(nullptr,&accelerationView,cpu(kSceneDescriptor));
            if (slot->characters) accelerationView.RaytracingAccelerationStructure.Location=slot->characters->acceleration->GetGPUVirtualAddress();
            device->CreateShaderResourceView(nullptr,&accelerationView,cpu(kCharacterSceneDescriptor));
            const auto characterView = slot->characters ? slot->characters->Reflections() : CharacterCasterFrame::ReflectionView{};
            const auto* actor = characterView.actor;
            const auto& actorEvaluation = characterView.evaluation ? *characterView.evaluation : sceneEvaluation;
            actorEvaluation.WriteTextureViews(device, cpu(kCharacterTextureOffset));
            const auto projection=slot->screenSpaceReflections && slot->material->projection && slot->material->projectionLookup &&
                slot->material->projection->Ready(stamp)?slot->material->projection:nullptr;
            WriteProjectionViews(device,projection.get(),cpu(5+NativePBR::kGpuTextureDescriptorCapacity));
            auto tokenView = depthView;
            tokenView.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
            device->CreateShaderResourceView(nullptr, &tokenView, cpu(4 + NativePBR::kGpuTextureDescriptorCapacity));
            ID3D12DescriptorHeap* heaps[]{ slot->descriptors.get() };
            cmd->SetDescriptorHeaps(1, heaps);
            const auto project = [&](const std::shared_ptr<const EnvironmentCube>& cube, UINT descriptor) {
                auto sh = std::make_shared<EnvironmentIrradiance>();
                sh->cube = cube; sh->allocationBytes = shAllocation;
                sh->coefficients = Eval::Buffer(device, kSHBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                auto* resource = cube->resource.get();
                Transition(cmd, resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                cmd->SetPipelineState(projectionPipeline.get()); cmd->SetComputeRootSignature(projectionRoot.get());
                cmd->SetComputeRootDescriptorTable(0, gpu(descriptor));
                cmd->SetComputeRootUnorderedAccessView(1, sh->coefficients->GetGPUVirtualAddress());
                cmd->SetComputeRoot32BitConstant(2, uint32_t(cube->stamp.encoding), 0);
                cmd->Dispatch(1, 1, 1);
                Transition(cmd, sh->coefficients.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                Transition(cmd, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
                return sh;
            };
            slot->baseSH = needsBaseSH ? (projectBase ? project(env.base, 1) : baseCache) : nullptr;
            slot->skySH = needsSkySH ? (projectSky ? project(env.sky, 3) : skyCache) : nullptr;
            const Constants constants{ raster.camera.CameraProjInverse, raster.camera.CameraViewInverse,
                width, height, slot->material->rowPitch, slot->rowPitch, enabled, available, env.base ? env.base->mipCount : 0u,
                env.reflection ? env.reflection->mipCount : 0u, environmentScale, skyScale,
                env.stamp.interior ? 1u : 0u, uint32_t(env.stamp.encoding), ambient,
                raster.camera.CameraPosAdjust, scene ? sceneEvaluation.instanceCount : 0u, slot->characters ? 1u : 0u, slot->quality, slot->frame,
                raster.camera.CameraProj, raster.camera.CameraView, slot->screenSpaceReflections ? 1u : 0u,
                slot->screenSpaceReflections ? slot->ssrDirect->rowPitch : 0u, slot->material->rayIdentityRowPitch,
                0u,
                slot->ssrDirect ? slot->ssrDirect->directional.direction : float4{},
                slot->ssrDirect ? slot->ssrDirect->directional.radiance : float4{},
                slot->ssrDirect ? slot->ssrDirect->lightCount : 0u,
                slot->ssrDirect && slot->ssrDirect->foliageScattering ? 1u : 0u,
                slot->skySH ? 1u : 0u,
                uint32_t(slot->material->parallaxEnabled) | (uint32_t(slot->material->parallaxShadows)<<1u),
                projection?projection->rowCount:0u, actor?actor->vertices:0u, actor?actor->triangles:0u, actor?actor->parts:0u, characterView.moverCount, slot->material->materialLobeRowPitch };
            void* mapped = nullptr; const D3D12_RANGE noRead{};
            Eval::Check(slot->constants->Map(0, &noRead, &mapped));
            std::memcpy(mapped, &constants, sizeof(constants)); slot->constants->Unmap(0, nullptr);
            auto* base = env.base ? env.base->resource.get() : nullptr;
            auto* reflection = env.reflection ? env.reflection->resource.get() : base;
            Transition(cmd, depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (base) Transition(cmd, base, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (reflection != base) Transition(cmd, reflection, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(cmd, slot->samples.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetPipelineState(pipeline.get()); cmd->SetComputeRootSignature(root.get());
            const auto materialAddress = slot->material->samples->GetGPUVirtualAddress();
            cmd->SetComputeRootShaderResourceView(0, materialAddress);
            cmd->SetComputeRootDescriptorTable(1, gpu(0));
            cmd->SetComputeRootShaderResourceView(2, slot->baseSH ? slot->baseSH->coefficients->GetGPUVirtualAddress() : materialAddress);
            cmd->SetComputeRootShaderResourceView(3, slot->skySH ? slot->skySH->coefficients->GetGPUVirtualAddress() : materialAddress);
            cmd->SetComputeRootUnorderedAccessView(4, slot->samples->GetGPUVirtualAddress());
            cmd->SetComputeRootConstantBufferView(5, slot->constants->GetGPUVirtualAddress());
            cmd->SetComputeRootDescriptorTable(6,gpu(kSceneDescriptor));
            cmd->SetComputeRootShaderResourceView(7, scene ? scene->shadingGeometry->buffer->GetGPUVirtualAddress() : materialAddress);
            cmd->SetComputeRootShaderResourceView(8, sceneEvaluation.records->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(9, sceneEvaluation.bindings->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(10, slot->material->specializedDirectory->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(22, slot->material->specializedPool->GetGPUVirtualAddress());
            const uint32_t specializedConstants[]{slot->material->specializedCapacity,slot->material->HasSpecialized()?1u:0u};
            cmd->SetComputeRoot32BitConstants(23,2,specializedConstants,0);
            cmd->SetComputeRootDescriptorTable(11,gpu(kCharacterSceneDescriptor));
            cmd->SetComputeRootShaderResourceView(12, slot->screenSpaceReflections ? slot->ssrDirect->samples->GetGPUVirtualAddress() : materialAddress);
            cmd->SetComputeRootShaderResourceView(13, slot->material->rayIdentity->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(14, materialAddress);
            cmd->SetComputeRootShaderResourceView(15, slot->ssrDirect ? slot->ssrDirect->lights->GetGPUVirtualAddress() : materialAddress);
            cmd->SetComputeRootShaderResourceView(16,projection?slot->material->projectionLookup->GetGPUVirtualAddress():materialAddress);
            cmd->SetComputeRootShaderResourceView(17,actor?actor->positions->GetGPUVirtualAddress():materialAddress);
            cmd->SetComputeRootShaderResourceView(18,actor?actor->indices->GetGPUVirtualAddress():materialAddress);
            cmd->SetComputeRootShaderResourceView(19,actorEvaluation.records->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(20,actorEvaluation.bindings->GetGPUVirtualAddress());
            cmd->SetComputeRootShaderResourceView(21,characterView.movers?characterView.movers->GetGPUVirtualAddress():materialAddress);
            TransitionProjectionViews(cmd,projection.get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cmd->Dispatch((ShadowPhysicalWidth(ShadowLogicalExtent(width, slot->quality), slot->quality) + 7) / 8,
                (ShadowLogicalExtent(height, slot->quality) + 7) / 8, 1);
            TransitionProjectionViews(cmd,projection.get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
            Transition(cmd, slot->samples.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            Transition(cmd, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            if (base) Transition(cmd, base, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            if (reflection != base) Transition(cmd, reflection, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Eval::Check(cmd->Close());
            Eval::Check(backend.commandQueue->Wait(slot->material->producerFence.get(), slot->material->producerValue));
            if (slot->characters) Eval::Check(backend.commandQueue->Wait(slot->characters->readyFence.get(), slot->characters->readyValue));
            Eval::Check(backend.commandQueue->Wait(env.producerFence.get(), env.producerValue));
            // Sky has a separate acquisition fence; base/reflection use the completed generation.
            if (env.sky) Eval::Check(backend.commandQueue->Wait(env.sky->readyFence.get(), env.sky->readyValue));
            if (projection) Eval::Check(backend.commandQueue->Wait(projection->readyFence.get(),projection->readyValue));
            if (slot->screenSpaceReflections) {
                Eval::Check(backend.commandQueue->Wait(slot->ssrDirect->readyFence.get(), slot->ssrDirect->readyValue));
            }
            ID3D12CommandList* lists[]{ cmd };
            backend.commandQueue->ExecuteCommandLists(1, lists); submitted = true;
            const uint64_t value = nextFence + 1;
            Eval::Check(backend.commandQueue->Signal(completion.get(), value));
            nextFence = value; slot->readyValue = value;
            if (slot->baseSH) baseCache = slot->baseSH;
            if (slot->skySH) skyCache = slot->skySH;
            current = slot; ++submissions; projections += uint64_t(projectBase) + uint64_t(projectSky);
            failure.clear();
        } catch (const std::exception& e) {
            failure = e.what();
            if (!selected || !*selected) return;
            if (!submitted) selected->reset();
            else {
                const uint64_t rescue = nextFence + 1;
                if (SUCCEEDED(backend.commandQueue->Signal(completion.get(), rescue))) {
                    nextFence = rescue; (*selected)->readyValue = rescue;
                }
            }
        }
    }
}

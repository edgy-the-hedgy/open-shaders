#include "DX12NativeComposite.h"

#include "DX12Control.h"
#include "Features/LightLimitFix/ShadowCasterManager.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "GpuPass.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <directx/d3dx12.h>
#include <fstream>
#include <ranges>
#include <format>
#include <string>
#include <vector>

namespace
{
	struct alignas(16) MaskCamera
	{
		Matrix projectionInverse, viewInverse;
		float4 adjust;
		uint32_t width, height, count;
		float shadowIntensity = 1.0f;
		uint32_t samplingMode = 0, samplingPhase = 0;
		uint32_t spatialReconstruction = 1, batchIndex = 0;
	};
	struct alignas(16) DiagnosticOptions
	{
		uint32_t width = 0, height = 0;
		float darkening = 1.0f;
		uint32_t batchCount = 0;
		int32_t selectedLight = -1; // -1: union across every prepared emitter
		uint32_t padding[3]{};
	};
	struct GpuLight
	{
		float4 positionRadius;
		float4 colorFade;
		float4 emitterFalloff;
	};
	static_assert(sizeof(MaskCamera) == 176);
	static_assert(sizeof(GpuLight) == 48);
	static_assert(sizeof(DiagnosticOptions) == 32);

	std::vector<char> ReadShader(const char* path)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file)
			return {};
		const auto size = file.tellg();
		if (size <= 0 || size > 16 * 1024 * 1024)
			return {};
		std::vector<char> bytes(size_t(size), '\0');
		file.seekg(0);
		return file.read(bytes.data(), std::streamsize(bytes.size())) ? bytes : std::vector<char>{};
	}

	bool CreateSignature(ID3D12Device* device, std::span<const D3D12_ROOT_PARAMETER> parameters,
		winrt::com_ptr<ID3D12RootSignature>& output, std::string& reason)
	{
		D3D12_ROOT_SIGNATURE_DESC desc{};
		desc.NumParameters = UINT(parameters.size());
		desc.pParameters = parameters.data();
		winrt::com_ptr<ID3DBlob> serialized, errors;
		const HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
			serialized.put(), errors.put());
		if (FAILED(hr)) {
			std::string diagnostic;
			if (errors && errors->GetBufferPointer() && errors->GetBufferSize()) {
				const auto length = std::min<size_t>(errors->GetBufferSize(), 512);
				diagnostic.assign(static_cast<const char*>(errors->GetBufferPointer()), length);
			}
			reason = std::format("Root signature serialization failed (0x{:08X}): {}",
				static_cast<uint32_t>(hr), diagnostic);
			return false;
		}
		const HRESULT create = device->CreateRootSignature(0, serialized->GetBufferPointer(),
			serialized->GetBufferSize(), IID_PPV_ARGS(output.put()));
		if (FAILED(create)) {
			reason = std::format("D3D12 CreateRootSignature failed (0x{:08X})",
				static_cast<uint32_t>(create));
			return false;
		}
		return true;
	}

	bool CreatePipeline(ID3D12Device* device, ID3D12RootSignature* root, const char* filename,
		winrt::com_ptr<ID3D12PipelineState>& output, std::string& reason)
	{
		const auto bytes = ReadShader(filename);
		if (bytes.empty()) {
			reason = std::format("Compiled DXIL is missing or unreadable: {}. Rebuild and deploy the AIO shader payload.", filename);
			return false;
		}
		D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
		pipeline.pRootSignature = root;
		pipeline.CS = { bytes.data(), bytes.size() };
		const HRESULT create = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(output.put()));
		if (FAILED(create)) {
			reason = std::format("D3D12 CreateComputePipelineState failed (0x{:08X}); DXIL bytes: {}",
				static_cast<uint32_t>(create), bytes.size());
			return false;
		}
		return true;
	}
}

DX12NativeComposite::~DX12NativeComposite()
{
    pipelineWorker.request_stop();
    if (pipelineWorker.joinable()) pipelineWorker.join();
    finishedColor.reset();
    for (auto& slot : compositeSlots) {
        if (!slot || !slot->frame || !slot->frame->producerFence) continue;
        const auto done = slot->frame->producerFence->GetCompletedValue();
        const bool pending = done == UINT64_MAX || done < slot->frame->producerValue ||
            std::ranges::any_of(slot->consumers, [](const auto& c) {
                const auto done = c.first->GetCompletedValue();
                return done == UINT64_MAX || done < c.second;
            });
        if (pending) (void)new std::shared_ptr<CompositeSlot>(std::move(slot));
    }
}

bool DX12NativeComposite::IsActive() const
{
    return loaded && !globals::game::isVR &&
        globals::features::dx12Control.IsActive() &&
        globals::features::dx12NativeScene.IsActive();
}

void DX12NativeComposite::DrawSettings()
{
    bool reconstruct = spatialReconstruction.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Depth-aware spatial shadow reconstruction", &reconstruct))
        spatialReconstruction.store(reconstruct, std::memory_order_release);
    float strength = shadowIntensity.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("Native shadow contribution", &strength, 0.0f, 4.0f, "%.2fx"))
        shadowIntensity.store(strength, std::memory_order_release);
    float blend = shadowBlend.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("Native shadow blend", &blend, 0.0f, 1.0f, "%.2f"))
        shadowBlend.store(blend, std::memory_order_release);
    bool preview = diagnosticDarkenEnabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Preview reconstructed native shadowmask", &preview))
        diagnosticDarkenEnabled.store(preview, std::memory_order_release);
    if (preview) {
        int light = diagnosticSelectedLight.load(std::memory_order_relaxed);
        const auto source = globals::features::dx12NativeScene.GetLights();
        const int maximum = std::max(0, int(source ? source->count : 0u) - 1);
        if (ImGui::SliderInt("Prepared light (-1 = all)", &light, -1, maximum))
            diagnosticSelectedLight.store(light, std::memory_order_release);
        ImGui::TextDisabled("-1 averages occlusion across covered prepared lights.");
    }
    bool staticFlicker = includeFlickeringLights.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Prepared flicker/pulse: force static shadow", &staticFlicker))
        includeFlickeringLights.store(staticFlicker, std::memory_order_release);
    bool suppress = disableMatchedRasterStatic.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Experimental: suppress matched raster static casters", &suppress))
        disableMatchedRasterStatic.store(suppress, std::memory_order_release);
    if (suppress)
        ImGui::TextDisabled("No coverage certification. Unrepresented raster casters may lose shadows.");
    const auto frame = globals::game::graphicsState ?
        globals::game::graphicsState->GetFrameCount() : 0u;
    const auto raw = globals::features::dx12NativeScene.GetRawShadow(frame);
    const auto final = GetFinishedColor(frame);
    ImGui::Text("Native shadow producer: %s", raw ? "Submitted" : "Waiting for scene/depth/lights");
    ImGui::Text("Prepared lights in raw mask: %u", raw ? raw->lightCount : 0u);
    ImGui::Text("Raw shadow batches: %u", raw ? raw->batchCount : 0u);
    ImGui::Text("Native Composite final color: %s", final ? "Published" : "Not published");
    ImGui::Text("Raster static caster skips: %llu",
        static_cast<unsigned long long>(ShadowCasterManager::GetNativePreviewStaticSkipCount()));
    if (pipelineFailed.load(std::memory_order_acquire)) {
        const auto reason = pipelineFailureReason.load(std::memory_order_acquire);
        ImGui::TextWrapped("Reconstruction shader unavailable: %s",
            reason ? reason->c_str() : "Unknown shader error");
    }
    ImGui::TextDisabled("RayQuery runs in Native Scene. Final color is composed before upscaling.");
}

bool DX12NativeComposite::MatchesNativeShadowMode(const RE::BSShadowLight* light) const
{
    return globals::features::dx12NativeScene.IsMatchedLightInShadowMode(light,
        true, includeFlickeringLights.load(std::memory_order_acquire));
}

bool DX12NativeComposite::ShouldDisableMatchedRasterShadows() const
{
    // An opt-in raster classifier decision only. No frame/geometry/light
    // revisions, coverage certification, diagnostic, or FSR admission.
    return IsActive() && disableMatchedRasterStatic.load(std::memory_order_acquire) &&
        ShadowCasterManager::IsActive();
}

void DX12NativeComposite::LoadSettings(json& input)
{
    const float intensity = input.value("ShadowMaskIntensity", 1.0f);
    shadowIntensity.store(std::clamp(std::isfinite(intensity) ? intensity : 1.0f,
        0.0f, 4.0f), std::memory_order_release);
    shadowBlend.store(std::clamp(input.value("ShadowBlend", 1.0f),
        0.0f, 1.0f), std::memory_order_release);
    spatialReconstruction.store(input.value("SpatialReconstruction", true),
        std::memory_order_release);
    diagnosticDarkenEnabled.store(input.value("DiagnosticDarken", false),
        std::memory_order_release);
    diagnosticSelectedLight.store(std::max(-1, input.value("DiagnosticSelectedLight", -1)),
        std::memory_order_release);
    disableMatchedRasterStatic.store(input.value("DisableMatchedRasterStatic", false),
        std::memory_order_release);
    includeFlickeringLights.store(input.value("ForcePreparedFlickerStatic", false),
        std::memory_order_release);
}

void DX12NativeComposite::SaveSettings(json& output)
{
    output = {
        {"SpatialReconstruction", spatialReconstruction.load(std::memory_order_acquire)},
        {"ShadowMaskIntensity", shadowIntensity.load(std::memory_order_acquire)},
        {"ShadowBlend", shadowBlend.load(std::memory_order_acquire)},
        {"DiagnosticDarken", diagnosticDarkenEnabled.load(std::memory_order_acquire)},
        {"DiagnosticSelectedLight", diagnosticSelectedLight.load(std::memory_order_acquire)},
        {"DisableMatchedRasterStatic", disableMatchedRasterStatic.load(std::memory_order_acquire)},
        {"ForcePreparedFlickerStatic", includeFlickeringLights.load(std::memory_order_acquire)}
    };
}

void DX12NativeComposite::RestoreDefaultSettings()
{
    shadowIntensity.store(1.0f, std::memory_order_release);
    shadowBlend.store(1.0f, std::memory_order_release);
    spatialReconstruction.store(true, std::memory_order_release);
    diagnosticDarkenEnabled.store(false, std::memory_order_release);
    diagnosticSelectedLight.store(-1, std::memory_order_release);
    disableMatchedRasterStatic.store(false, std::memory_order_release);
    includeFlickeringLights.store(false, std::memory_order_release);
}

void DX12NativeComposite::PreShadowMaps()
{
    // SCM's broad whole-light suppression is not used for this independent layer.
    ShadowCasterManager::SetNativeStaticMaskActive(false);
    const bool armed = ShouldDisableMatchedRasterShadows();
    if (armed != previousRasterSuppressionArmed) {
        if (const auto matched = globals::features::dx12NativeScene.GetMatchedLightPointers();
            matched && !matched->empty())
            ShadowCasterManager::InvalidateNativePreviewStaticBakes(*matched);
        previousRasterSuppressionArmed = armed;
    }
    if (IsActive())
        (void)EnsurePipelines(globals::features::upscaling.dx12SwapChain.d3d12Device.get());
}

void DX12NativeComposite::OnRuntimeDisabled()
{
    ShadowCasterManager::SetNativeStaticMaskActive(false);
    finishedColor.reset();
    previousRasterSuppressionArmed = false;
}

void DX12NativeComposite::OnSceneTransitionReset(bool /*opening*/)
{
    // Current TLAS remains retained until the next native scene publication.
}

bool DX12NativeComposite::EnsurePipelines(ID3D12Device* candidate)
{
    if (!candidate) return false;
    if (reconstructionPipeline && device.get() == candidate) return true;
    if (const auto built = preparedPipelines.load(std::memory_order_acquire)) {
        if (built->device.get() != candidate) return false;
        device = built->device;
        reconstructionRoot = built->reconstructionRoot;
        reconstructionPipeline = built->reconstructionPipeline;
        return true;
    }
    if (pipelineFailed.load(std::memory_order_acquire)) return false;
    if (pipelineWorker.joinable()) return false;
    winrt::com_ptr<ID3D12Device> retained;
    retained.copy_from(candidate);
    pipelineWorker = std::jthread([this, held = std::move(retained)](std::stop_token stop) mutable {
        auto built = std::make_shared<PipelineState>();
        built->device = std::move(held);
        D3D12_DESCRIPTOR_RANGE depth{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0};
        D3D12_DESCRIPTOR_RANGE raw{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 0, 0};
        D3D12_DESCRIPTOR_RANGE output{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
        std::array<D3D12_ROOT_PARAMETER, 4> params{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = {1, &depth};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &raw};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {1, &output};
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[3].Descriptor.ShaderRegister = 0;
        std::string failure;
        if (!CreateSignature(built->device.get(), params, built->reconstructionRoot, failure) ||
            !CreatePipeline(built->device.get(), built->reconstructionRoot.get(),
                "Data/Shaders/DX12NativeComposite/ReconstructShadowMask.dxil",
                built->reconstructionPipeline, failure)) {
            pipelineFailureReason.store(std::make_shared<const std::string>(std::move(failure)),
                std::memory_order_release);
            pipelineFailed.store(true, std::memory_order_release);
            return;
        }
        if (!stop.stop_requested())
            preparedPipelines.store(std::move(built), std::memory_order_release);
    });
    return false;
}

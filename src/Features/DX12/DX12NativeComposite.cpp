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
		uint32_t spatialReconstruction = 1, padding1 = 0;
	};
	struct alignas(16) DiagnosticOptions
	{
		uint32_t width = 0, height = 0;
		float darkening = 1.0f, padding = 0.0f;
	};
	struct GpuLight
	{
		float4 positionRadius;
		float4 colorFade;
		float4 emitterFalloff;
	};
	static_assert(sizeof(MaskCamera) == 176);
	static_assert(sizeof(GpuLight) == 48);
	static_assert(sizeof(DiagnosticOptions) == 16);

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
	if (pipelineWorker.joinable())
		pipelineWorker.join();
	Collect();
	for (auto& frame : pending)
		if (frame.fence && frame.fence->GetCompletedValue() < frame.value)
			(void)new Pending(std::move(frame));
}

bool DX12NativeComposite::IsActive() const
{
	return loaded && !globals::game::isVR &&
	       globals::features::dx12Control.IsActive() && globals::features::dx12NativeScene.IsActive();
}

void DX12NativeComposite::DrawSettings()
{
	const char* modes[] = {
		"Full resolution (1/1)", "Two-thirds (2/3)", "Half checkerboard (1/2)",
		"Four-ninths (4/9)", "One-third (1/3)", "Quarter (1/4)",
		"One-sixth (1/6)", "One-ninth (1/9)"
	};
	int mode = std::clamp(samplingMode.load(std::memory_order_relaxed), 0, 7);
	if (ImGui::Combo("Shadowmask ray sampling", &mode, modes, int(std::size(modes)))) {
		samplingMode.store(mode, std::memory_order_release);
		samplingPhase.store(0, std::memory_order_release);
		lastValidDiagnosticFrame.store(0, std::memory_order_release);
	}
	bool spatial = spatialReconstruction.load(std::memory_order_relaxed);
	if (ImGui::Checkbox("Spatial shadow reconstruction (depth-aware)", &spatial))
		spatialReconstruction.store(spatial, std::memory_order_release);
	// Stationary prepared lights are always eligible. Authored flicker/pulse
	// remains entirely vanilla unless explicitly forced to a static native shadow.
	bool forceStatic = includeFlickeringLights.load(std::memory_order_relaxed);
	if (ImGui::Checkbox("Prepared flicker/pulse: force static native shadow", &forceStatic)) {
		includeFlickeringLights.store(forceStatic, std::memory_order_release);
		lastNativeShadowFrame.store(0, std::memory_order_release);
	}
	ImGui::TextDisabled("Off = preserve vanilla flickering shadow behavior; no moving-light observer is installed.");
	float intensity = shadowIntensity.load(std::memory_order_relaxed);
	if (ImGui::SliderFloat("Shadowmask intensity", &intensity, 0.0f, 4.0f, "%.2fx"))
		shadowIntensity.store(intensity, std::memory_order_release);
	bool show = diagnosticDarkenEnabled.load(std::memory_order_relaxed);
	if (ImGui::Checkbox("Diagnostic: monochrome shadow visibility", &show)) {
		diagnosticDarkenEnabled.store(show, std::memory_order_release);
		lastValidDiagnosticFrame.store(0, std::memory_order_release);
	}
	if (show) {
		float power = diagnosticDarkenStrength.load(std::memory_order_relaxed);
		if (ImGui::SliderFloat("Visibility contrast", &power, 0.0f, 4.0f, "%.2fx"))
			diagnosticDarkenStrength.store(power, std::memory_order_release);
	}
	ImGui::TextDisabled("Debug shows monochrome occlusion; never multiplies raster lighting.");
	bool rasterOff = disableMatchedRasterStatic.load(std::memory_order_relaxed);
	if (ImGui::Checkbox("Test: disable raster shadows for matched static geometry", &rasterOff)) {
		disableMatchedRasterStatic.store(rasterOff, std::memory_order_release);
		if (!rasterOff)
			lastNativeShadowFrame.store(0, std::memory_order_release);
	}
	if (rasterOff) {
		ImGui::TextDisabled("Requires a recent native light-space atlas, published TLAS, matched geometry, and SCM.");
		ImGui::TextDisabled("Only live-captured rigid meshes in the current TLAS are eligible.");
		ImGui::TextDisabled("EXPERIMENTAL: native direct-light composition is absent; missing raster shadows possible.");
		ImGui::Text("Raster offload: %s", ShouldDisableMatchedRasterShadows() ? "Armed" : "Waiting for safe conditions");
	}
	ImGui::Text("Runtime: %s", IsActive() ? "Automatic (DX12 master enabled)" : "Inactive");
	ImGui::Text("Shadowmask submissions: %llu",
		static_cast<unsigned long long>(submittedMasks.load(std::memory_order_acquire)));
	ImGui::Text("Independent receiver RayQuery frames: %llu",
		static_cast<unsigned long long>(submittedMasks.load(std::memory_order_relaxed)));
	ImGui::Text("Diagnostic FSR submissions: %llu",
		static_cast<unsigned long long>(submittedDiagnosticFrames.load(std::memory_order_relaxed)));
	ImGui::Text("Raster static caster append skips: %llu",
		static_cast<unsigned long long>(ShadowCasterManager::GetNativePreviewStaticSkipCount()));
	ImGui::Text("Runtime FSR dispatch callbacks: %llu",
		static_cast<unsigned long long>(dispatchCalls.load(std::memory_order_relaxed)));
	ImGui::Text("Prepared native lights: %u (stationary %u | prepared flicker/pulse %u)",
		lastPreparedLightCount.load(std::memory_order_relaxed),
		lastStationaryCount.load(std::memory_order_relaxed),
		lastFlickeringCount.load(std::memory_order_relaxed));
	const char* lastStatus = "Unknown";
	switch (lastDispatchStatus.load(std::memory_order_acquire)) {
	case DispatchStatus::NeverCalled: lastStatus = "Waiting for runtime FSR upscaler callback"; break;
	case DispatchStatus::InactiveOrInputsMissing: lastStatus = "Inactive or missing DX12 inputs"; break;
	case DispatchStatus::PipelineNotReady: lastStatus = "DXR pipeline pending or failed"; break;
	case DispatchStatus::SceneNotReady: lastStatus = "Current TLAS, lights or grid unavailable"; break;
	case DispatchStatus::NoPreparedLights: lastStatus = "No valid prepared lights"; break;
	case DispatchStatus::SurfacesUnavailable: lastStatus = "Depth/color surface unavailable"; break;
	case DispatchStatus::UploadBudgetExhausted: lastStatus = "Upload pairs awaiting GPU fences"; break;
	case DispatchStatus::UploadFailed: lastStatus = "GPU upload allocation failed"; break;
	case DispatchStatus::Recorded: lastStatus = "Mask recorded, pending GPU submission"; break;
	case DispatchStatus::Submitted: lastStatus = "Mask submitted on DX12 queue"; break;
	}
	ImGui::TextWrapped("Last mask dispatch: %s", lastStatus);
	if (pipelineFailed.load(std::memory_order_acquire)) {
		const auto failure = pipelineFailureReason.load(std::memory_order_acquire);
		ImGui::TextWrapped("DXR shadowmask initialization failed: %s",
			failure ? failure->c_str() : "Unknown D3D12 initialization error");
	}
	if (show && diagnosticUnavailable.load(std::memory_order_acquire)) {
		const auto failure = diagnosticFailureReason.load(std::memory_order_acquire);
		ImGui::TextWrapped("Diagnostic overlay unavailable: %s", failure ? failure->c_str() : "Unsupported color format");
	}
	ImGui::TextDisabled("Prepared lights and TLAS remain camera-independent; receiver queries use current depth.");
	ImGui::TextDisabled("Sampling modes control per-frame RayQuery density; reconstruction fills skipped receivers.");
	ImGui::TextDisabled("Current dispatch remains on the runtime FSR DX12 callback.");
	ImGui::TextDisabled("Intensity: 0 = neutral, 1 = raw, above 1 deepens partial shadows.");
	ImGui::TextDisabled("With diagnostic OFF: original raster color is unchanged until safe per-light composition is available.");
}

bool DX12NativeComposite::MatchesNativeShadowMode(const RE::BSShadowLight* light) const
{
	return globals::features::dx12NativeScene.IsMatchedLightInShadowMode(light,
		includeStationaryLights.load(std::memory_order_acquire),
		includeFlickeringLights.load(std::memory_order_acquire));
}

bool DX12NativeComposite::ShouldDisableMatchedRasterShadows() const
{
	if (!disableMatchedRasterStatic.load(std::memory_order_acquire) || !IsActive() ||
		(!includeStationaryLights.load(std::memory_order_acquire) &&
		 !includeFlickeringLights.load(std::memory_order_acquire)) ||
		pipelineFailed.load(std::memory_order_acquire) ||
		!ShadowCasterManager::IsActive())
		return false;
	auto* state = globals::game::graphicsState;
	if (!state)
		return false;
	const uint32_t last = lastNativeShadowFrame.load(std::memory_order_acquire);
	const uint32_t now = state->GetFrameCount();
	const auto scene = globals::features::dx12NativeScene.GetScene();
	const auto grid = globals::features::dx12Resources.GetGrid();
	if (!scene || !scene->acceleration || !grid ||
		scene->geometryRevision != grid->geometryRevision ||
		scene->geometryRevision != lastNativeGeometryRevision.load(std::memory_order_acquire) ||
		grid->lightRevision != lastNativeLightRevision.load(std::memory_order_acquire))
		return false;
	// This experimental pre-raster filter uses a recently submitted world-space
	// native shadow atlas as its readiness signal, not native direct lighting,
	// diagnostic darkening, camera orientation or a chosen sampling mode.
	return last && now >= last && now - last <= 2;
}

void DX12NativeComposite::LoadSettings(json& input)
{
	const int mode = std::clamp(input.value("SamplingMode", 0), 0, 7);
	const float requested = input.value("ShadowMaskIntensity", 1.0f);
	const float intensity = std::clamp(std::isfinite(requested) ? requested : 1.0f, 0.0f, 4.0f);
	samplingMode.store(mode, std::memory_order_release);
	shadowIntensity.store(intensity, std::memory_order_release);
	diagnosticDarkenEnabled.store(input.value("DiagnosticDarken", false), std::memory_order_release);
	const float test = input.value("DiagnosticDarkening", 1.0f);
	diagnosticDarkenStrength.store(std::clamp(std::isfinite(test) ? test : 1.0f, 0.0f, 4.0f), std::memory_order_release);
	disableMatchedRasterStatic.store(input.value("DisableMatchedRasterStatic", false), std::memory_order_release);
	includeStationaryLights.store(true, std::memory_order_release);
	includeFlickeringLights.store(input.value("ForcePreparedFlickerStatic", false), std::memory_order_release);
	lastNativeShadowFrame.store(0, std::memory_order_release);
	spatialReconstruction.store(input.value("SpatialReconstruction", true), std::memory_order_release);
	samplingPhase.store(0, std::memory_order_release);
}

void DX12NativeComposite::SaveSettings(json& output)
{
	output = { { "SamplingMode", samplingMode.load(std::memory_order_acquire) },
		{ "SpatialReconstruction", spatialReconstruction.load(std::memory_order_acquire) },
		{ "ShadowMaskIntensity", shadowIntensity.load(std::memory_order_acquire) },
		{ "DiagnosticDarken", diagnosticDarkenEnabled.load(std::memory_order_acquire) },
		{ "DiagnosticDarkening", diagnosticDarkenStrength.load(std::memory_order_acquire) },
		{ "DisableMatchedRasterStatic", disableMatchedRasterStatic.load(std::memory_order_acquire) },
		{ "ForcePreparedFlickerStatic", includeFlickeringLights.load(std::memory_order_acquire) } };
}

void DX12NativeComposite::RestoreDefaultSettings()
{
	samplingMode.store(0, std::memory_order_release);
	spatialReconstruction.store(true, std::memory_order_release);
	shadowIntensity.store(1.0f, std::memory_order_release);
	diagnosticDarkenEnabled.store(false, std::memory_order_release);
	diagnosticDarkenStrength.store(1.0f, std::memory_order_release);
	disableMatchedRasterStatic.store(false, std::memory_order_release);
	includeStationaryLights.store(true, std::memory_order_release);
	includeFlickeringLights.store(false, std::memory_order_release);
	lastNativeShadowFrame.store(0, std::memory_order_release);
	lastValidDiagnosticFrame.store(0, std::memory_order_release);
	samplingPhase.store(0, std::memory_order_release);
}

void DX12NativeComposite::PreShadowMaps()
{
	// Keep SCM's broad whole-light offload disabled. Experimental suppression
	// operates on individually verified, static runtime geometry only.
	ShadowCasterManager::SetNativeStaticMaskActive(false);
	const bool armed = ShouldDisableMatchedRasterShadows();
	const auto currentScene = globals::features::dx12NativeScene.GetScene();
	const uint64_t revision = currentScene ? currentScene->geometryRevision : 0;
	const uint32_t policy = (includeStationaryLights.load(std::memory_order_acquire) ? 1u : 0u) |
		(includeFlickeringLights.load(std::memory_order_acquire) ? 2u : 0u);
	if (armed != previousRasterSuppressionArmed ||
		(armed && (revision != previousRasterRefreshRevision || policy != previousRasterPolicyFlags))) {
		// Flush any baked static content whenever the experimental filter starts,
		// stops, or changes geometry generation. Otherwise old static atlas data
		// could remain despite successfully filtering new caster draws.
		if (const auto matched = globals::features::dx12NativeScene.GetMatchedLightPointers();
			matched && !matched->empty())
			ShadowCasterManager::InvalidateNativePreviewStaticBakes(*matched);
		previousRasterSuppressionArmed = armed;
		previousRasterRefreshRevision = revision;
		previousRasterPolicyFlags = policy;
	}
	if (IsActive())
		(void)EnsurePipelines(globals::features::upscaling.dx12SwapChain.d3d12Device.get());
	Collect();
}

void DX12NativeComposite::OnRuntimeDisabled()
{
	ShadowCasterManager::SetNativeStaticMaskActive(false);
	lastValidDiagnosticFrame.store(0, std::memory_order_release);
	lastNativeShadowFrame.store(0, std::memory_order_release);
	previousRasterSuppressionArmed = false;
	previousRasterRefreshRevision = 0;
	previousRasterPolicyFlags = 0;
	lastNativeGeometryRevision.store(0, std::memory_order_release);
	lastNativeLightRevision.store(0, std::memory_order_release);
}

void DX12NativeComposite::OnSceneTransitionReset(bool opening)
{
	if (opening)
		OnRuntimeDisabled();
}

bool DX12NativeComposite::EnsurePipelines(ID3D12Device* candidate)
{
    if (!candidate || unavailable || (device && device.get() != candidate)) return false;
    if (shadowPipeline && reconstructionPipeline) return true;
    if (const auto built = preparedPipelines.load(std::memory_order_acquire)) {
        if (built->device.get() != candidate) return false;
        device = built->device;
        shadowRoot = built->shadowRoot;
        shadowPipeline = built->shadowPipeline;
        reconstructionRoot = built->reconstructionRoot;
        reconstructionPipeline = built->reconstructionPipeline;
        diagnosticRoot = built->diagnosticRoot;
        diagnosticPipeline = built->diagnosticPipeline;
        descriptorSize = built->descriptorSize;
        return true;
    }
    if (pipelineFailed.load(std::memory_order_acquire)) { unavailable = true; return false; }
    if (pipelineWorker.joinable()) return false;
    winrt::com_ptr<ID3D12Device> held;
    held.copy_from(candidate);
    pipelineWorker = std::jthread([this, owned = std::move(held)](std::stop_token stop) mutable {
        auto built = std::make_shared<PipelineState>();
        built->device = std::move(owned);
        D3D12_DESCRIPTOR_RANGE depthRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
        D3D12_DESCRIPTOR_RANGE rawRead{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 0, 0 };
        D3D12_DESCRIPTOR_RANGE filteredRead{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
        D3D12_DESCRIPTOR_RANGE writeRange{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
        std::array<D3D12_ROOT_PARAMETER, 5> shadow{};
        shadow[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        shadow[0].Descriptor.ShaderRegister = 0;
        shadow[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        shadow[1].DescriptorTable = { 1, &depthRange };
        shadow[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        shadow[2].Descriptor.ShaderRegister = 2;
        shadow[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        shadow[3].DescriptorTable = { 1, &writeRange };
        shadow[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        shadow[4].Descriptor.ShaderRegister = 0;
        std::array<D3D12_ROOT_PARAMETER, 4> reconstruct{};
        reconstruct[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        reconstruct[0].DescriptorTable = { 1, &depthRange };
        reconstruct[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        reconstruct[1].DescriptorTable = { 1, &rawRead };
        reconstruct[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        reconstruct[2].DescriptorTable = { 1, &writeRange };
        reconstruct[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        reconstruct[3].Descriptor.ShaderRegister = 0;
        std::array<D3D12_ROOT_PARAMETER, 3> diagnostic{};
        diagnostic[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        diagnostic[0].DescriptorTable = { 1, &filteredRead };
        diagnostic[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        diagnostic[1].DescriptorTable = { 1, &writeRange };
        diagnostic[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        diagnostic[2].Descriptor.ShaderRegister = 0;
        std::string failure;
        if (!CreateSignature(built->device.get(), shadow, built->shadowRoot, failure) ||
            !CreatePipeline(built->device.get(), built->shadowRoot.get(),
                "Data/Shaders/DX12NativeComposite/PreparedShadowMask.dxil", built->shadowPipeline, failure) ||
            !CreateSignature(built->device.get(), reconstruct, built->reconstructionRoot, failure) ||
            !CreatePipeline(built->device.get(), built->reconstructionRoot.get(),
                "Data/Shaders/DX12NativeComposite/ReconstructShadowMask.dxil", built->reconstructionPipeline, failure)) {
            pipelineFailureReason.store(std::make_shared<const std::string>(std::move(failure)), std::memory_order_release);
            pipelineFailed.store(true, std::memory_order_release);
            return;
        }
        if (!CreateSignature(built->device.get(), diagnostic, built->diagnosticRoot, failure) ||
            !CreatePipeline(built->device.get(), built->diagnosticRoot.get(),
                "Data/Shaders/DX12NativeComposite/VisualizeShadowMask.dxil", built->diagnosticPipeline, failure)) {
            diagnosticFailureReason.store(std::make_shared<const std::string>(std::move(failure)), std::memory_order_release);
            diagnosticUnavailable.store(true, std::memory_order_release);
        }
        built->descriptorSize = built->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (!built->descriptorSize) {
            pipelineFailureReason.store(std::make_shared<const std::string>("Zero descriptor increment"), std::memory_order_release);
            pipelineFailed.store(true, std::memory_order_release);
            return;
        }
        if (!stop.stop_requested()) preparedPipelines.store(std::move(built), std::memory_order_release);
    });
    return false;
}

std::shared_ptr<DX12NativeComposite::Surfaces> DX12NativeComposite::MakeSurfaces(
    ID3D12Resource* rasterColor, ID3D12Resource* depth, uint32_t width, uint32_t height)
{
    const auto raster = rasterColor->GetDesc();
    const auto z = depth->GetDesc();
    const DXGI_FORMAT depthSrv = z.Format == DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_R32_FLOAT : z.Format;
    if (!width || !height || raster.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        z.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || width > raster.Width || height > raster.Height ||
        width > z.Width || height > z.Height || depthSrv != DXGI_FORMAT_R32_FLOAT ||
        raster.Format == DXGI_FORMAT_UNKNOWN) return nullptr;
    bool wantDebug = diagnosticDarkenEnabled.load(std::memory_order_acquire) &&
        diagnosticPipeline && !diagnosticUnavailable.load(std::memory_order_acquire);
    if (wantDebug) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
        support.Format = raster.Format;
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
            !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
            diagnosticFailureReason.store(std::make_shared<const std::string>("Debug color UAV format not supported"), std::memory_order_release);
            diagnosticUnavailable.store(true, std::memory_order_release);
            wantDebug = false;
        }
    }
    if (surfaces && surfaces->rasterSource.get() == rasterColor && surfaces->depthSource.get() == depth &&
        surfaces->width == width && surfaces->height == height &&
        surfaces->rasterFormat == raster.Format && surfaces->depthFormat == z.Format &&
        bool(surfaces->diagnosticColor) == wantDebug) return surfaces;
    auto next = std::make_shared<Surfaces>();
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto bitsDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_UINT,
        width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bitsDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(next->rawShadowMask.put()))) ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bitsDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(next->shadowMask.put())))) return nullptr;
    next->rawShadowMask->SetName(L"DX12NativeComposite::RawPerLightShadowBits");
    next->shadowMask->SetName(L"DX12NativeComposite::ReconstructedPerLightShadowBits");
    next->rasterSource.copy_from(rasterColor);
    next->depthSource.copy_from(depth);
    if (wantDebug) {
        const auto debugDesc = CD3DX12_RESOURCE_DESC::Tex2D(raster.Format, raster.Width, raster.Height,
            1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &debugDesc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(next->diagnosticColor.put())))) {
            diagnosticUnavailable.store(true, std::memory_order_release);
            wantDebug = false;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC descriptorDesc{};
    descriptorDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptorDesc.NumDescriptors = wantDebug ? 6 : 5;
    descriptorDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&descriptorDesc, IID_PPV_ARGS(next->descriptors.put())))) return nullptr;
    const auto cpu = [&](uint32_t index) {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(next->descriptors->GetCPUDescriptorHandleForHeapStart(), int(index), descriptorSize);
    };
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    srv.Format = depthSrv;
    device->CreateShaderResourceView(depth, &srv, cpu(0));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    device->CreateUnorderedAccessView(next->rawShadowMask.get(), nullptr, &uav, cpu(1));
    srv.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    device->CreateShaderResourceView(next->rawShadowMask.get(), &srv, cpu(2));
    device->CreateUnorderedAccessView(next->shadowMask.get(), nullptr, &uav, cpu(3));
    device->CreateShaderResourceView(next->shadowMask.get(), &srv, cpu(4));
    if (wantDebug) {
        uav.Format = raster.Format;
        device->CreateUnorderedAccessView(next->diagnosticColor.get(), nullptr, &uav, cpu(5));
    }
    next->width = width;
    next->height = height;
    next->rasterFormat = raster.Format;
    next->depthFormat = z.Format;
    surfaces = next;
    return next;
}

bool DX12NativeComposite::Upload(winrt::com_ptr<ID3D12Resource>& buffer, const void* source,
	size_t bytes, size_t capacity)
{
	if (!source || !bytes || bytes > capacity || capacity > 64 * 1024)
		return false;
	if (!buffer) {
		const auto desc = CD3DX12_RESOURCE_DESC::Buffer((capacity + 255) & ~size_t(255));
		const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(buffer.put()))))
			return false;
	}
	void* mapped = nullptr;
	if (FAILED(buffer->Map(0, nullptr, &mapped)) || !mapped)
		return false;
	std::memcpy(mapped, source, bytes);
	buffer->Unmap(0, nullptr);
	return true;
}

D3D12_GPU_DESCRIPTOR_HANDLE DX12NativeComposite::Descriptor(const Surfaces& target, uint32_t index) const
{
	return CD3DX12_GPU_DESCRIPTOR_HANDLE(target.descriptors->GetGPUDescriptorHandleForHeapStart(), int(index), descriptorSize);
}

void DX12NativeComposite::Collect()
{
	while (!pending.empty() && pending.front().fence &&
		   pending.front().fence->GetCompletedValue() >= pending.front().value) {
		auto& completed = pending.front().submission;
		availableUploads.push_back({ std::move(completed.camera), std::move(completed.lights), std::move(completed.diagnostic) });
		pending.pop_front();
	}
}

std::optional<DX12NativeComposite::Submission> DX12NativeComposite::Dispatch(
    ID3D12GraphicsCommandList4* commands, ID3D12Resource* rasterColor, ID3D12Resource* depth,
    uint32_t width, uint32_t height)
{
    CS_GPU_PASS("DX12NativeComposite::Dispatch");
    dispatchCalls.fetch_add(1, std::memory_order_relaxed);
    lastPreparedLightCount.store(0, std::memory_order_relaxed);
    lastStationaryCount.store(0, std::memory_order_relaxed);
    lastFlickeringCount.store(0, std::memory_order_relaxed);
    if (!IsActive() || !commands || !rasterColor || !depth) {
        lastDispatchStatus.store(DispatchStatus::InactiveOrInputsMissing, std::memory_order_release);
        return std::nullopt;
    }
    if (!EnsurePipelines(globals::features::upscaling.dx12SwapChain.d3d12Device.get())) {
        lastDispatchStatus.store(DispatchStatus::PipelineNotReady, std::memory_order_release);
        return std::nullopt;
    }
    const auto scene = globals::features::dx12NativeScene.GetScene();
    const auto lightSnapshot = globals::features::dx12NativeScene.GetLights();
    const auto grid = globals::features::dx12Resources.GetGrid();
    if (!scene || !scene->acceleration || !lightSnapshot || !grid ||
        scene->scope != grid->requested.front().scope) {
        lastDispatchStatus.store(DispatchStatus::SceneNotReady, std::memory_order_release);
        return std::nullopt;
    }
    std::array<GpuLight, DX12::kMaximumShadowLights> lightUpload{};
    const bool stationary = includeStationaryLights.load(std::memory_order_acquire);
    const bool flickering = includeFlickeringLights.load(std::memory_order_acquire);
    uint32_t count = 0, stationaryCount = 0, flickeringCount = 0;
    for (uint32_t index = 0; index < lightSnapshot->count; ++index) {
        const auto& resolved = lightSnapshot->values[index];
        const auto& light = resolved.light;
        if (resolved.flickeringOrMoving ? !flickering : !stationary) continue;
        if (!light.referenceID || !std::isfinite(light.radius) || light.radius <= 4.0f ||
            !std::isfinite(light.emitterRadius) || !std::isfinite(light.falloffExponent) ||
            !std::isfinite(light.position.x) || !std::isfinite(light.position.y) ||
            !std::isfinite(light.position.z)) continue;
        if (resolved.flickeringOrMoving) ++flickeringCount; else ++stationaryCount;
        lightUpload[count++] = { {light.position.x, light.position.y, light.position.z, light.radius},
            {light.color.x, light.color.y, light.color.z, light.fade},
            {light.emitterRadius, light.falloffExponent, 0, 0} };
    }
    lastPreparedLightCount.store(count, std::memory_order_relaxed);
    lastStationaryCount.store(stationaryCount, std::memory_order_relaxed);
    lastFlickeringCount.store(flickeringCount, std::memory_order_relaxed);
    if (!count) {
        lastDispatchStatus.store(DispatchStatus::NoPreparedLights, std::memory_order_release);
        return std::nullopt;
    }
    Collect();
    auto target = MakeSurfaces(rasterColor, depth, width, height);
    if (!target) {
        lastDispatchStatus.store(DispatchStatus::SurfacesUnavailable, std::memory_order_release);
        return std::nullopt;
    }
    const MaskCamera camera{globals::game::frameBufferCached.GetCameraProjInverse(),
        globals::game::frameBufferCached.GetCameraViewInverse(),
        globals::game::frameBufferCached.GetCameraPosAdjust(), width, height, count,
        shadowIntensity.load(std::memory_order_acquire),
        uint32_t(std::clamp(samplingMode.load(std::memory_order_acquire), 0, 7)),
        samplingPhase.load(std::memory_order_relaxed),
        spatialReconstruction.load(std::memory_order_acquire) ? 1u : 0u};
    if (availableUploads.empty() && uploadPairs == kMaximumUploadPairs) {
        lastDispatchStatus.store(DispatchStatus::UploadBudgetExhausted, std::memory_order_release);
        return std::nullopt;
    }
    UploadPair buffers;
    if (!availableUploads.empty()) {
        buffers = std::move(availableUploads.front());
        availableUploads.pop_front();
    } else ++uploadPairs;
    const bool makeDiagnostic = target->diagnosticColor && diagnosticPipeline;
    const DiagnosticOptions diagnostic{width, height,
        diagnosticDarkenStrength.load(std::memory_order_acquire), 0.0f};
    if (!Upload(buffers.camera, &camera, sizeof(camera), sizeof(camera)) ||
        !Upload(buffers.lights, lightUpload.data(), count * sizeof(GpuLight), sizeof(lightUpload)) ||
        (makeDiagnostic && !Upload(buffers.diagnostic, &diagnostic, sizeof(diagnostic), sizeof(diagnostic)))) {
        availableUploads.push_back(std::move(buffers));
        lastDispatchStatus.store(DispatchStatus::UploadFailed, std::memory_order_release);
        return std::nullopt;
    }
    Submission submission;
    submission.scene = scene;
    submission.surfaces = target;
    submission.geometryRevision = scene->geometryRevision;
    submission.lightRevision = grid->lightRevision;
    submission.camera = std::move(buffers.camera);
    submission.lights = std::move(buffers.lights);
    submission.diagnostic = std::move(buffers.diagnostic);
    ID3D12DescriptorHeap* heaps[]{target->descriptors.get()};
    commands->SetDescriptorHeaps(1, heaps);
    const auto beginRaw = CD3DX12_RESOURCE_BARRIER::Transition(target->rawShadowMask.get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commands->ResourceBarrier(1, &beginRaw);
    commands->SetComputeRootSignature(shadowRoot.get());
    commands->SetPipelineState(shadowPipeline.get());
    commands->SetComputeRootShaderResourceView(0, scene->acceleration->GetGPUVirtualAddress());
    commands->SetComputeRootDescriptorTable(1, Descriptor(*target,0));
    commands->SetComputeRootShaderResourceView(2, submission.lights->GetGPUVirtualAddress());
    commands->SetComputeRootDescriptorTable(3, Descriptor(*target,1));
    commands->SetComputeRootConstantBufferView(4, submission.camera->GetGPUVirtualAddress());
    commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    // The sparse raw field is read only after the full RayQuery dispatch.
    std::array<D3D12_RESOURCE_BARRIER, 2> beginReconstruction{
        CD3DX12_RESOURCE_BARRIER::Transition(target->rawShadowMask.get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(target->shadowMask.get(),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    commands->ResourceBarrier(UINT(beginReconstruction.size()), beginReconstruction.data());
    commands->SetComputeRootSignature(reconstructionRoot.get());
    commands->SetPipelineState(reconstructionPipeline.get());
    commands->SetComputeRootDescriptorTable(0, Descriptor(*target,0));
    commands->SetComputeRootDescriptorTable(1, Descriptor(*target,2));
    commands->SetComputeRootDescriptorTable(2, Descriptor(*target,3));
    commands->SetComputeRootConstantBufferView(3, submission.camera->GetGPUVirtualAddress());
    commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    const auto rawDone = CD3DX12_RESOURCE_BARRIER::Transition(target->rawShadowMask.get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    commands->ResourceBarrier(1, &rawDone);
    samplingPhase.fetch_add(1, std::memory_order_relaxed);
    if (makeDiagnostic) {
        std::array<D3D12_RESOURCE_BARRIER, 2> beginVisual{
            CD3DX12_RESOURCE_BARRIER::Transition(target->shadowMask.get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(target->diagnosticColor.get(),
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        commands->ResourceBarrier(UINT(beginVisual.size()), beginVisual.data());
        commands->SetComputeRootSignature(diagnosticRoot.get());
        commands->SetPipelineState(diagnosticPipeline.get());
        commands->SetComputeRootDescriptorTable(0, Descriptor(*target,4));
        commands->SetComputeRootDescriptorTable(1, Descriptor(*target,5));
        commands->SetComputeRootConstantBufferView(2, submission.diagnostic->GetGPUVirtualAddress());
        commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        std::array<D3D12_RESOURCE_BARRIER, 2> endVisual{
            CD3DX12_RESOURCE_BARRIER::Transition(target->shadowMask.get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            CD3DX12_RESOURCE_BARRIER::Transition(target->diagnosticColor.get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        commands->ResourceBarrier(UINT(endVisual.size()), endVisual.data());
        submission.usedDiagnostic = true;
    } else {
        const auto shadowDone = CD3DX12_RESOURCE_BARRIER::Transition(target->shadowMask.get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        commands->ResourceBarrier(1, &shadowDone);
    }
    lastDispatchStatus.store(DispatchStatus::Recorded, std::memory_order_release);
    return submission;
}

void DX12NativeComposite::OnSubmitted(ID3D12Fence* fence, uint64_t value, Submission submission, bool upscalerSucceeded)
{
	if (!fence || !value)
		return;
	// A recorded native shadow mask is independent of diagnostic visualization.
	lastNativeGeometryRevision.store(submission.geometryRevision, std::memory_order_release);
	lastNativeLightRevision.store(submission.lightRevision, std::memory_order_release);
	if (globals::game::graphicsState)
		lastNativeShadowFrame.store(globals::game::graphicsState->GetFrameCount(), std::memory_order_release);
	if (upscalerSucceeded && submission.usedDiagnostic) {
		submittedDiagnosticFrames.fetch_add(1, std::memory_order_relaxed);
		if (globals::game::graphicsState)
			lastValidDiagnosticFrame.store(globals::game::graphicsState->GetFrameCount(), std::memory_order_release);
	}
	Pending frame;
	frame.fence.copy_from(fence);
	frame.value = value;
	frame.submission = std::move(submission);
	pending.push_back(std::move(frame));
	submittedMasks.fetch_add(1, std::memory_order_relaxed);
	lastDispatchStatus.store(DispatchStatus::Submitted, std::memory_order_release);
	Collect();
}

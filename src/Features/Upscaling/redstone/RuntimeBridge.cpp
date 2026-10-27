#include "Features/Upscaling.h"
#include "RuntimeDenoiser.h"
#include "Util.h"

bool RedstoneDenoiser::Initialize()
{
	const auto directory = (Util::PathHelpers::GetDataPath() / "Shaders" / "Upscaling" / "FidelityFX").lexically_normal();
	if (!directory.is_absolute())
		return false;
	const auto load = [&](const wchar_t* a_name) {
		return LoadLibraryExW((directory / a_name).c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	};
	loaderModule = load(L"amd_fidelityfx_loader_dx12.dll");
	if (loaderModule)
		ffxLoadFunctions(&denoiserFunctions, loaderModule);
	if (!denoiserFunctions.CreateContext || !denoiserFunctions.DestroyContext || !denoiserFunctions.Dispatch ||
		!denoiserFunctions.Configure || !denoiserFunctions.Query) {
		logger::warn("[Redstone] Denoiser loader unavailable; retaining original SSGI");
		return false;
	}
	denoiserModule = load(L"amd_fidelityfx_denoiser_dx12.dll");
	featureRayRegeneration = denoiserModule && EnsureRuntimeUpscalerInterop();
	return featureRayRegeneration;
}

RedstoneDenoiser::~RedstoneDenoiser()
{
	ResetRayRegeneration();
	if (rayRegenerationContext || rrSharedSignal) {
		for (auto& context : runtimeCommandContexts) {
			(void)context.commandList.detach();
			(void)context.commandAllocator.detach();
		}
		(void)runtimeFence.fence11.detach();
		(void)runtimeFence.fence12.detach();
		return;
	}
	if (denoiserModule)
		FreeLibrary(denoiserModule);
	if (loaderModule)
		FreeLibrary(loaderModule);
}

bool RedstoneDenoiser::EnsureRuntimeUpscalerInterop()
{
	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	if (!swapChain.d3d11Device || !swapChain.d3d11Context || !swapChain.d3d12Device || !swapChain.commandQueue)
		return false;
	try {
		if (!runtimeFence.fence12)
			runtimeFence.Create(swapChain.d3d12Device.get(), swapChain.d3d11Device.get(), "Redstone::RRFence");
		for (auto& context : runtimeCommandContexts) {
			if (context.commandList)
				continue;
			DX::ThrowIfFailed(swapChain.d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(context.commandAllocator.put())));
			DX::ThrowIfFailed(swapChain.d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, context.commandAllocator.get(), nullptr, IID_PPV_ARGS(context.commandList.put())));
			DX::ThrowIfFailed(context.commandList->Close());
		}
		return true;
	} catch (const std::exception& e) {
		rayRegenerationFailureLatched = true;
		logger::error("[Redstone] RR interop unavailable: {}", e.what());
		return false;
	}
}

RedstoneDenoiser::RuntimeCommandContext* RedstoneDenoiser::AcquireRuntimeCommandContext()
{
	const auto completed = runtimeFence.fence12->GetCompletedValue();
	if (completed == UINT64_MAX) {
		rayRegenerationFailureLatched = true;
		return nullptr;
	}
	for (uint32_t i = 0; i < runtimeCommandContexts.size(); ++i) {
		const auto index = (runtimeCommandContextCursor + i) % runtimeCommandContexts.size();
		auto& context = runtimeCommandContexts[index];
		if (!context.quarantined && context.fenceValue <= completed) {
			runtimeCommandContextCursor = static_cast<uint32_t>((index + 1) % runtimeCommandContexts.size());
			return &context;
		}
	}
	return nullptr;
}

json RedstoneDenoiser::GetRayRegenerationDiagnostics() const
{
	json signals = { "indirectDiffuse", "indirectSpecular" };
	if (rayRegenerationOcclusionSignals & FFX_DENOISER_SIGNAL_AMBIENT_OCCLUSION) signals.push_back("ambientOcclusion");
	if (rayRegenerationOcclusionSignals & FFX_DENOISER_SIGNAL_SPECULAR_OCCLUSION) signals.push_back("specularOcclusion");
	return {
		{ "providerLoaded", featureRayRegeneration },
		{ "failureLatched", rayRegenerationFailureLatched },
		{ "configuration", GetConfiguration() },
		{ "successfulDispatches", rayRegenerationSuccessfulDispatches },
		{ "failedDispatches", rayRegenerationFailedDispatches },
		{ "historyResets", rayRegenerationHistoryResets },
		{ "contextWidth", rayRegenerationMaxRenderSize[0] },
		{ "contextHeight", rayRegenerationMaxRenderSize[1] },
		{ "dispatchWidth", rayRegenerationWidth },
		{ "dispatchHeight", rayRegenerationHeight },
		{ "checkerboard", rayRegenerationCheckerboard },
		{ "signalFlags", (FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE | FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR) | rayRegenerationOcclusionSignals },
		{ "occlusionSignalFlags", rayRegenerationOcclusionSignals },
		{ "signals", signals },
		{ "signalWidth", rayRegenerationCheckerboard ? rayRegenerationWidth / 2u : rayRegenerationWidth }
	};
}

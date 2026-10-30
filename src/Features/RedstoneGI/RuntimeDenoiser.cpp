#include "RuntimeDenoiser.h"
#include "Utils/RayRegenerationSettings.h"
#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Util.h"
#include <directx/d3dx12.h>

namespace
{
	constexpr float kMinimumRayRegenerationDepth = 0.001f;
	constexpr uint32_t kIndirectLightingSignals = FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE | FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR;
	constexpr uint32_t kDirectLightingSignals = FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE | FFX_DENOISER_SIGNAL_DIRECT_SPECULAR;

	/** @brief Builds a single-mip interop texture descriptor with the requested extent and bindings. */
	D3D11_TEXTURE2D_DESC SharedDesc(ID3D11Texture2D* source, uint32_t width, uint32_t height, UINT bind)
	{
		D3D11_TEXTURE2D_DESC d{};
		source->GetDesc(&d);
		d.Width = width;
		d.Height = height;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.SampleDesc = { 1, 0 };
		d.Usage = D3D11_USAGE_DEFAULT;
		d.CPUAccessFlags = 0;
		d.BindFlags = bind;
		d.MiscFlags = 0;
		return d;
	}

	/** @brief Deletes an owned wrapped resource and clears its pointer. */
	void DeleteWrapped(WrappedResource*& p)
	{
		delete p;
		p = nullptr;
	}

	/** @brief Converts an engine shader matrix to FidelityFX row-major matrix storage. */
	FfxApiMatrix4x4 ToFfxMatrix(const Matrix& source)
	{
		const auto m = source.Transpose();
		FfxApiMatrix4x4 out{};
		static_assert(sizeof(out) == sizeof(m));
		std::memcpy(&out, &m, sizeof(out));
		return out;
	}

	/** @brief Calls the denoiser provider behind an SEH boundary and reports provider faults. */
	ffxReturnCode_t DispatchDenoiserProtected(const ffxFunctions& denoiserFunctions, ffxContext* context, const ffxDispatchDescHeader* desc, bool& faulted)
	{
		faulted = false;
		ffxReturnCode_t result = FFX_API_RETURN_ERROR;
		__try {
			result = denoiserFunctions.Dispatch(context, desc);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			faulted = true;
		}
		return result;
	}
}

/** @brief Retires paired RR GPU work before releasing its context and wrapped textures. */
void RedstoneDenoiser::ResetRayRegeneration()
{
	if (rayRegenerationContext || rrSharedSignal) {
		auto& sc = backend;
		if (!sc.d3d11Context || !sc.commandQueue || !runtimeFence.fence11 || !runtimeFence.fence12) {
			rayRegenerationFailureLatched = true;
			logger::error("[RayRegeneration] Runtime bridge unavailable during teardown; preserving context and shared resources");
			return;
		}
		try {
			sc.d3d11Context->Flush();
			const uint64_t d3d12RetirementFence = runtimeFence.Next();
			DX::ThrowIfFailed(sc.commandQueue->Signal(runtimeFence.fence12.get(), d3d12RetirementFence));
			rayRegenerationLastFenceValue = d3d12RetirementFence;
			if (!WaitForRuntimeD3D12Fence(rayRegenerationLastFenceValue)) {
				rayRegenerationFailureLatched = true;
				logger::error("[RayRegeneration] D3D12 teardown timed out; preserving context and shared resources");
				return;
			}

			const uint64_t d3d11RetirementFence = runtimeFence.Next();
			DX::ThrowIfFailed(sc.d3d11Context->Signal(runtimeFence.fence11.get(), d3d11RetirementFence));
			sc.d3d11Context->Flush();
			if (!WaitForRuntimeD3D12Fence(d3d11RetirementFence)) {
				rayRegenerationFailureLatched = true;
				logger::error("[RayRegeneration] D3D11 teardown timed out; preserving context and shared resources");
				return;
			}
		} catch (const std::exception& e) {
			rayRegenerationFailureLatched = true;
			logger::error("[RayRegeneration] Teardown failed; preserving context and shared resources: {}", e.what());
			return;
		}
	}

	if (rayRegenerationContext && denoiserFunctions.DestroyContext)
		(void)denoiserFunctions.DestroyContext(&rayRegenerationContext, nullptr);
	rayRegenerationContext = nullptr;
	{
		std::scoped_lock lock(configurationMutex);
		configurationDefaultsReady = false;
		configurationDirty = true;
	}
	rayRegenerationLastFenceValue = 0;
	rayRegenerationWidth = rayRegenerationHeight = 0;
	rayRegenerationMaxRenderSize = {};
	DeleteWrapped(rrSharedSignal);
	DeleteWrapped(rrSharedDepth);
	DeleteWrapped(rrSharedMotion);
	DeleteWrapped(rrSharedNormal);
	DeleteWrapped(rrSharedSpecularAlbedo);
	DeleteWrapped(rrSharedDiffuseAlbedo);
	DeleteWrapped(rrSharedOutput);
	DeleteWrapped(rrSharedDiffuseSignal);
	DeleteWrapped(rrSharedDiffuseOutput);
	for (auto& resource : rrSharedOcclusionInput) DeleteWrapped(resource);
	for (auto& resource : rrSharedOcclusionOutput) DeleteWrapped(resource);
	rayRegenerationOcclusionSignals = 0;
}

/** @brief Sizes context capacity to the engine extent and interop textures to the active RR grid. */
bool RedstoneDenoiser::EnsureRayRegenerationResources(ID3D11Texture2D* signal, ID3D11Texture2D* depth, ID3D11Texture2D* motion,
	ID3D11Texture2D* normal, ID3D11Texture2D* specAlbedo, ID3D11Texture2D* diffAlbedo, ID3D11Texture2D* output,
	ID3D11Texture2D* diffuseSignal, ID3D11Texture2D* diffuseOutput,
	const OcclusionResources& occlusion, const std::array<uint32_t, 2>& engineResolution, uint32_t width, uint32_t height, bool checkerboard, bool directSignals)
{
	constexpr uint32_t supportedOcclusionSignals = FFX_DENOISER_SIGNAL_AMBIENT_OCCLUSION | FFX_DENOISER_SIGNAL_SPECULAR_OCCLUSION;
	if (directSignals && occlusion.signalFlags) return false;
	if (occlusion.signalFlags & ~supportedOcclusionSignals) return false;
	if (!CanDispatchRayRegeneration() || !denoiserModule || !denoiserFunctions.CreateContext || !denoiserFunctions.DestroyContext || !denoiserFunctions.Dispatch)
		return false;
	if (!engineResolution[0] || !engineResolution[1] || !width || !height || width > engineResolution[0] || height > engineResolution[1]) {
		rayRegenerationFailureLatched = true;
		logger::error("[RayRegeneration] Invalid RR grid {}x{} for engine rendering extent {}x{}; disabling RR for this session",
			width, height, engineResolution[0], engineResolution[1]);
		return false;
	}
	const std::array<ID3D11Texture2D*, 13> textures{ signal, depth, motion, normal, specAlbedo, diffAlbedo, output, diffuseSignal, diffuseOutput, occlusion.diffuseInput, occlusion.specularInput, occlusion.diffuseOutput, occlusion.specularOutput };
	for (size_t i = 0; i < textures.size(); ++i) {
		if (i >= 9 && !(occlusion.signalFlags & ((i == 9 || i == 11) ? FFX_DENOISER_SIGNAL_AMBIENT_OCCLUSION : FFX_DENOISER_SIGNAL_SPECULAR_OCCLUSION))) continue;
		D3D11_TEXTURE2D_DESC desc{};
		if (textures[i])
			textures[i]->GetDesc(&desc);
		const DXGI_FORMAT expectedFormat = i >= 9 ? DXGI_FORMAT_R8_UNORM : i == 1 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
		const uint32_t requiredWidth = (i == 0 || i == 7 || i == 9 || i == 10) && checkerboard ? width / 2u : width;
		if (!width || !height || (checkerboard && (width < 2u || (width & 1u))) || !textures[i] || desc.Width < requiredWidth || desc.Height < height ||
			desc.Format != expectedFormat || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0) {
			rayRegenerationFailureLatched = true;
			logger::error("[RayRegeneration] Invalid texture {}: {}x{}, format {}, array {}, samples {}:{}; expected at least {}x{} in format {} with one slice/sample; disabling RR for this session",
				i, desc.Width, desc.Height, static_cast<uint32_t>(desc.Format), desc.ArraySize, desc.SampleDesc.Count, desc.SampleDesc.Quality,
				width, height, static_cast<uint32_t>(expectedFormat));
			return false;
		}
	}
	if (!EnsureRuntimeUpscalerInterop())
		return false;

	if (rayRegenerationContext && rayRegenerationWidth == width && rayRegenerationHeight == height &&
		rayRegenerationMaxRenderSize == engineResolution &&
		rayRegenerationCheckerboard == checkerboard && rayRegenerationDirectSignals == directSignals && rayRegenerationOcclusionSignals == occlusion.signalFlags && rrSharedOutput && rrSharedDiffuseOutput)
		return true;

	ResetRayRegeneration();
	if (rayRegenerationContext || rayRegenerationFailureLatched)
		return false;
	auto& sc = backend;
	try {
		rrSharedSignal = new WrappedResource(SharedDesc(signal, checkerboard ? width / 2u : width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::Signal");
		rrSharedDiffuseSignal = new WrappedResource(SharedDesc(diffuseSignal, checkerboard ? width / 2u : width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::DiffuseSignal");
		rrSharedDepth = new WrappedResource(SharedDesc(depth, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::Depth");
		rrSharedMotion = new WrappedResource(SharedDesc(motion, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::Motion");
		rrSharedNormal = new WrappedResource(SharedDesc(normal, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::NormalRoughness");
		rrSharedSpecularAlbedo = new WrappedResource(SharedDesc(specAlbedo, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::SpecularAlbedo");
		rrSharedDiffuseAlbedo = new WrappedResource(SharedDesc(diffAlbedo, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::DiffuseAlbedo");
		rrSharedOutput = new WrappedResource(SharedDesc(output, width, height, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::Output");
		rrSharedDiffuseOutput = new WrappedResource(SharedDesc(diffuseOutput, width, height, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::DiffuseOutput");

		const std::array<ID3D11Texture2D*, 2> occlusionInputs{ occlusion.diffuseInput, occlusion.specularInput };
		const std::array<ID3D11Texture2D*, 2> occlusionOutputs{ occlusion.diffuseOutput, occlusion.specularOutput };
		for (uint32_t i = 0; i < 2; ++i) {
			if (!(occlusion.signalFlags & (i ? FFX_DENOISER_SIGNAL_SPECULAR_OCCLUSION : FFX_DENOISER_SIGNAL_AMBIENT_OCCLUSION))) continue;
			rrSharedOcclusionInput[i] = new WrappedResource(SharedDesc(occlusionInputs[i], checkerboard ? width / 2u : width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), i ? "RayRegeneration::SpecularOcclusionInput" : "RayRegeneration::DiffuseOcclusionInput");
			rrSharedOcclusionOutput[i] = new WrappedResource(SharedDesc(occlusionOutputs[i], width, height, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS), sc.d3d11Device.get(), sc.d3d12Device.get(), i ? "RayRegeneration::SpecularOcclusionOutput" : "RayRegeneration::DiffuseOcclusionOutput");
		}

		ffxCreateContextDescDenoiser create{};
		create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
		create.version = FFX_DENOISER_VERSION;
		create.maxRenderSize = { engineResolution[0], engineResolution[1] };
		create.signalFlags = (directSignals ? kDirectLightingSignals : kIndirectLightingSignals) | occlusion.signalFlags;
		create.checkerboardSignalFlags = checkerboard ? create.signalFlags : 0;
		create.flags = 0;
		ffx::CreateBackendDX12Desc backendDesc{};
		backendDesc.device = sc.d3d12Device.get();
		create.header.pNext = &backendDesc.header;
		if (denoiserFunctions.CreateContext(&rayRegenerationContext, &create.header, nullptr) != FFX_API_RETURN_OK || !rayRegenerationContext) {
			logger::error("[RayRegeneration] FidelityFX Denoiser 1.2 context creation failed; disabling RR for this session");
			rayRegenerationFailureLatched = true;
			ResetRayRegeneration();
			return false;
		}
		rayRegenerationWidth = width;
		rayRegenerationHeight = height;
		rayRegenerationMaxRenderSize = engineResolution;
		rayRegenerationCheckerboard = checkerboard;
		rayRegenerationDirectSignals = directSignals;
		rayRegenerationOcclusionSignals = occlusion.signalFlags;
		logger::info("[RayRegeneration] FidelityFX Denoiser 1.2 context capacity {}x{}; RR grid {}x{}; {} diffuse + specular; checkerboard={}; occlusion signals={}",
			engineResolution[0], engineResolution[1], width, height, directSignals ? "direct" : "indirect", checkerboard, occlusion.signalFlags);
		return true;
	} catch (const std::exception& e) {
		logger::error("[RayRegeneration] Resource/context setup failed: {}", e.what());
		ResetRayRegeneration();
		return false;
	}
}

/** @brief Reconstructs both indirect signals and copies their synchronized results to D3D11. */
bool RedstoneDenoiser::DispatchRayRegeneration(ID3D11Texture2D* signal, ID3D11Texture2D* depth, ID3D11Texture2D* motion,
	ID3D11Texture2D* normal, ID3D11Texture2D* specAlbedo, ID3D11Texture2D* diffAlbedo, ID3D11Texture2D* output,
	ID3D11Texture2D* diffuseSignal, ID3D11Texture2D* diffuseOutput,
	const OcclusionResources& occlusion, const std::array<uint32_t, 2>& engineResolution, uint32_t width, uint32_t height, bool resetHistory, bool checkerboard, uint32_t checkerboardOrigin, bool directSignals)
{
	CS_GPU_PASS_SELECT(directSignals, "RedstoneShadows::RuntimeDirectDispatch", "Redstone::RuntimeIndirectDispatch");
	const bool engineExtentChanged = rayRegenerationMaxRenderSize != engineResolution || rayRegenerationOcclusionSignals != occlusion.signalFlags || rayRegenerationDirectSignals != directSignals;
	if (!EnsureRayRegenerationResources(signal, depth, motion, normal, specAlbedo, diffAlbedo, output, diffuseSignal, diffuseOutput, occlusion, engineResolution, width, height, checkerboard, directSignals))
		return false;
	resetHistory |= engineExtentChanged;
	const auto& configuration = RayRegeneration::GetConfiguration();
	if (sharedConfigurationRevision != configuration.revision) {
		SetConfiguration(configuration.overrides);
		sharedConfigurationRevision = configuration.revision;
	}
	if (!ApplyConfiguration(resetHistory))
		return false;
	auto& sc = backend;
	auto* cc = AcquireRuntimeCommandContext();
	if (!cc)
		return false;

	D3D11_BOX box{ 0, 0, 0, width, height, 1 };
	auto copy11 = [&](ID3D11Texture2D* src, WrappedResource* dst) { sc.d3d11Context->CopySubresourceRegion(dst->resource11, 0, 0, 0, 0, src, 0, &box); };
	bool commandListRecording = false;
	try {
		D3D11_BOX signalBox = box;
		signalBox.right = checkerboard ? width / 2u : width;
		sc.d3d11Context->CopySubresourceRegion(rrSharedSignal->resource11, 0, 0, 0, 0, signal, 0, &signalBox);
		sc.d3d11Context->CopySubresourceRegion(rrSharedDiffuseSignal->resource11, 0, 0, 0, 0, diffuseSignal, 0, &signalBox);
		if (occlusion.signalFlags & FFX_DENOISER_SIGNAL_AMBIENT_OCCLUSION)
			sc.d3d11Context->CopySubresourceRegion(rrSharedOcclusionInput[0]->resource11, 0, 0, 0, 0, occlusion.diffuseInput, 0, &signalBox);
		if (occlusion.signalFlags & FFX_DENOISER_SIGNAL_SPECULAR_OCCLUSION)
			sc.d3d11Context->CopySubresourceRegion(rrSharedOcclusionInput[1]->resource11, 0, 0, 0, 0, occlusion.specularInput, 0, &signalBox);
		copy11(depth, rrSharedDepth);
		copy11(motion, rrSharedMotion);
		copy11(normal, rrSharedNormal);
		copy11(specAlbedo, rrSharedSpecularAlbedo);
		copy11(diffAlbedo, rrSharedDiffuseAlbedo);
		const uint64_t f11 = runtimeFence.Next();
		DX::ThrowIfFailed(sc.d3d11Context->Signal(runtimeFence.fence11.get(), f11));
		DX::ThrowIfFailed(sc.commandQueue->Wait(runtimeFence.fence12.get(), f11));

		DX::ThrowIfFailed(cc->commandAllocator->Reset());
		DX::ThrowIfFailed(cc->commandList->Reset(cc->commandAllocator.get(), nullptr));
		commandListRecording = true;
		std::vector<ID3D12Resource*> reads = { rrSharedSignal->resource.get(), rrSharedDiffuseSignal->resource.get(), rrSharedDepth->resource.get(), rrSharedMotion->resource.get(), rrSharedNormal->resource.get(), rrSharedSpecularAlbedo->resource.get(), rrSharedDiffuseAlbedo->resource.get() };
		for (auto* resource : rrSharedOcclusionInput) if (resource) reads.push_back(resource->resource.get());
		std::vector<D3D12_RESOURCE_BARRIER> begin;
		for (auto* r : reads) begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(rrSharedOutput->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(rrSharedDiffuseOutput->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		for (auto* resource : rrSharedOcclusionOutput)
			if (resource) begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		cc->commandList->ResourceBarrier((UINT)begin.size(), begin.data());

		ffxDispatchDescDenoiser common{};
		common.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
		common.commandList = cc->commandList.get();
		common.linearDepth = ffxApiGetResourceDX12(rrSharedDepth->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		common.motionVectors = ffxApiGetResourceDX12(rrSharedMotion->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		common.normals = ffxApiGetResourceDX12(rrSharedNormal->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		common.specularAlbedo = ffxApiGetResourceDX12(rrSharedSpecularAlbedo->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		common.diffuseAlbedo = ffxApiGetResourceDX12(rrSharedDiffuseAlbedo->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		common.motionVectorScale = { 1.f, 1.f, 1.f };
		const auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);
		const auto currentBackend = Feature::FindGraphicsBackend();
		const auto jitter = currentBackend ? currentBackend->jitter : backend.jitter;
		common.jitterOffsets = {
			-jitter.x * static_cast<float>(width) / std::max(1.0f, renderSize.x),
			-jitter.y * static_cast<float>(height) / std::max(1.0f, renderSize.y)
		};
		const auto& curPos = globals::game::frameBufferCached.GetCameraPosAdjust();
		const auto& prevPos = globals::game::frameBufferCached.GetCameraPreviousPosAdjust();
		common.cameraPositionDelta = { prevPos.x - curPos.x, prevPos.y - curPos.y, prevPos.z - curPos.z };
		common.view = ToFfxMatrix(globals::game::frameBufferCached.GetCameraView());
		common.projection = ToFfxMatrix(globals::game::frameBufferCached.GetCameraProjUnjittered());
		common.linearDepthBounds = { std::max(kMinimumRayRegenerationDepth, *globals::game::cameraNear), *globals::game::cameraFar };
		common.renderSize = { width, height };
		common.frameIndex = (uint32_t)*globals::game::frameCounter;
		common.flags = FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO | (resetHistory ? FFX_DENOISER_DISPATCH_RESET : 0);

		ffxDispatchDescDenoiserIndirectSpecular spec{};
		spec.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR;
		spec.signal.input = ffxApiGetResourceDX12(rrSharedSignal->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		spec.signal.output = ffxApiGetResourceDX12(rrSharedOutput->resource.get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
		spec.signal.checkerboardOrigin = checkerboard ? checkerboardOrigin & 1u : 0u;
		ffxDispatchDescDenoiserIndirectDiffuse diffuse{};
		diffuse.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE;
		diffuse.signal.input = ffxApiGetResourceDX12(rrSharedDiffuseSignal->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
		diffuse.signal.output = ffxApiGetResourceDX12(rrSharedDiffuseOutput->resource.get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
		diffuse.signal.checkerboardOrigin = spec.signal.checkerboardOrigin;
		common.header.pNext = &diffuse.header;
		diffuse.header.pNext = &spec.header;
		ffxDispatchDescDenoiserDirectDiffuse directDiffuse{};
		directDiffuse.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE;
		directDiffuse.signal = diffuse.signal;
		ffxDispatchDescDenoiserDirectSpecular directSpecular{};
		directSpecular.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR;
		directSpecular.signal = spec.signal;
		if (directSignals) {
			common.header.pNext = &directDiffuse.header;
			directDiffuse.header.pNext = &directSpecular.header;
		}
		ffxDispatchDescDenoiserAmbientOcclusion diffuseOcclusion{};
		diffuseOcclusion.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_AMBIENT_OCCLUSION;

		ffxDispatchDescDenoiserSpecularOcclusion specularOcclusion{};
		specularOcclusion.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_SPECULAR_OCCLUSION;

		if (occlusion.signalFlags & FFX_DENOISER_SIGNAL_AMBIENT_OCCLUSION) {
			diffuseOcclusion.signal = { ffxApiGetResourceDX12(rrSharedOcclusionInput[0]->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ), ffxApiGetResourceDX12(rrSharedOcclusionOutput[0]->resource.get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV), spec.signal.checkerboardOrigin };
			diffuseOcclusion.header.pNext = spec.header.pNext;
			spec.header.pNext = &diffuseOcclusion.header;
		}
		if (occlusion.signalFlags & FFX_DENOISER_SIGNAL_SPECULAR_OCCLUSION) {
			specularOcclusion.signal = { ffxApiGetResourceDX12(rrSharedOcclusionInput[1]->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ), ffxApiGetResourceDX12(rrSharedOcclusionOutput[1]->resource.get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV), spec.signal.checkerboardOrigin };
			specularOcclusion.header.pNext = spec.header.pNext;
			spec.header.pNext = &specularOcclusion.header;
		}

		bool dispatchFaulted = false;
		const ffxReturnCode_t dispatchResult = DispatchDenoiserProtected(denoiserFunctions, &rayRegenerationContext, &common.header, dispatchFaulted);
		if (dispatchFaulted || dispatchResult != FFX_API_RETURN_OK) {
			++rayRegenerationFailedDispatches;
			logger::error("[RayRegeneration] Denoiser dispatch {}; disabling RR for this session", dispatchFaulted ? "faulted inside the provider" : "returned failure");
			rayRegenerationFailureLatched = true;

			if (commandListRecording) {
				const HRESULT closeResult = cc->commandList->Close();
				commandListRecording = false;
				if (FAILED(closeResult))
					logger::warn("[RayRegeneration] Failed to close abandoned D3D12 command list: 0x{:08X}", static_cast<unsigned>(closeResult));
			}
			return false;
		}

		std::vector<D3D12_RESOURCE_BARRIER> end;
		for (auto* r : reads) end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
		end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(rrSharedOutput->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
		end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(rrSharedDiffuseOutput->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
		for (auto* resource : rrSharedOcclusionOutput)
			if (resource) end.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON));
		cc->commandList->ResourceBarrier((UINT)end.size(), end.data());
		DX::ThrowIfFailed(cc->commandList->Close());
		commandListRecording = false;
		ID3D12CommandList* lists[] = { cc->commandList.get() };
		cc->quarantined = true;
		sc.commandQueue->ExecuteCommandLists(1, lists);
		const uint64_t f12 = runtimeFence.Next();
		DX::ThrowIfFailed(sc.commandQueue->Signal(runtimeFence.fence12.get(), f12));
		cc->fenceValue = f12;
		cc->quarantined = false;
		rayRegenerationLastFenceValue = f12;
		DX::ThrowIfFailed(sc.d3d11Context->Wait(runtimeFence.fence11.get(), f12));
		sc.d3d11Context->CopySubresourceRegion(output, 0, 0, 0, 0, rrSharedOutput->resource11, 0, &box);
		sc.d3d11Context->CopySubresourceRegion(diffuseOutput, 0, 0, 0, 0, rrSharedDiffuseOutput->resource11, 0, &box);
		if (occlusion.signalFlags & FFX_DENOISER_SIGNAL_AMBIENT_OCCLUSION)
			sc.d3d11Context->CopySubresourceRegion(occlusion.diffuseOutput, 0, 0, 0, 0, rrSharedOcclusionOutput[0]->resource11, 0, &box);
		if (occlusion.signalFlags & FFX_DENOISER_SIGNAL_SPECULAR_OCCLUSION)
			sc.d3d11Context->CopySubresourceRegion(occlusion.specularOutput, 0, 0, 0, 0, rrSharedOcclusionOutput[1]->resource11, 0, &box);

		++rayRegenerationSuccessfulDispatches;
		if (resetHistory) ++rayRegenerationHistoryResets;

		return true;
	} catch (const std::exception& e) {
		if (commandListRecording) {
			(void)cc->commandList->Close();
			commandListRecording = false;
		}
		++rayRegenerationFailedDispatches;
		rayRegenerationFailureLatched = true;
		logger::error("[RayRegeneration] Dispatch failed: {}", e.what());
		return false;
	}
}

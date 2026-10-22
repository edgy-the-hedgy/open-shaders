#include "../../../Globals.h"
#include "../../../GpuPass.h"
#include "../../../Util.h"
#include "../../Upscaling.h"
#include "../DX12SwapChain.h"
#include "../FidelityFX.h"

#include <directx/d3dx12.h>

extern ffxFunctions ffxModule;

namespace
{
	constexpr float kMinimumRayRegenerationDepth = 0.001f;

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
	ffxReturnCode_t DispatchDenoiserProtected(ffxContext* context, const ffxDispatchDescHeader* desc, bool& faulted)
	{
		faulted = false;
		ffxReturnCode_t result = FFX_API_RETURN_ERROR;
		__try {
			result = ffxModule.Dispatch(context, desc);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			faulted = true;
		}
		return result;
	}
}

/** @brief Retires indirect-specular RR GPU work before releasing its context and wrapped textures. */
void FidelityFX::ResetRayRegeneration()
{
	if (rayRegenerationContext || rrSharedSignal) {
		auto& sc = globals::features::upscaling.dx12SwapChain;
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

	if (rayRegenerationContext && ffxModule.DestroyContext)
		(void)ffxModule.DestroyContext(&rayRegenerationContext, nullptr);
	rayRegenerationContext = nullptr;
	rayRegenerationLastFenceValue = 0;
	rayRegenerationWidth = rayRegenerationHeight = 0;
	DeleteWrapped(rrSharedSignal);
	DeleteWrapped(rrSharedDepth);
	DeleteWrapped(rrSharedMotion);
	DeleteWrapped(rrSharedNormal);
	DeleteWrapped(rrSharedSpecularAlbedo);
	DeleteWrapped(rrSharedDiffuseAlbedo);
	DeleteWrapped(rrSharedOutput);
}

bool FidelityFX::CanDispatchRayRegeneration() const
{
	return featureRayRegeneration && !rayRegenerationFailureLatched && !globals::game::isVR &&
	       GetFsr4AdapterSupport() == Fsr4AdapterSupport::RadeonRx9000;
}

/** @brief Ensures indirect-specular RR context and shared textures match the requested render extent. */
bool FidelityFX::EnsureRayRegenerationResources(ID3D11Texture2D* signal, ID3D11Texture2D* depth, ID3D11Texture2D* motion,
	ID3D11Texture2D* normal, ID3D11Texture2D* specAlbedo, ID3D11Texture2D* diffAlbedo, ID3D11Texture2D* output,
	uint32_t width, uint32_t height)
{
	if (!CanDispatchRayRegeneration() || !denoiserModule || !ffxModule.CreateContext || !ffxModule.DestroyContext || !ffxModule.Dispatch)
		return false;
	const std::array<ID3D11Texture2D*, 7> textures{ signal, depth, motion, normal, specAlbedo, diffAlbedo, output };
	for (size_t i = 0; i < textures.size(); ++i) {
		D3D11_TEXTURE2D_DESC desc{};
		if (textures[i])
			textures[i]->GetDesc(&desc);
		const DXGI_FORMAT expectedFormat = i == 1 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
		if (!width || !height || !textures[i] || desc.Width < width || desc.Height < height ||
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

	if (rayRegenerationContext && rayRegenerationWidth == width && rayRegenerationHeight == height && rrSharedOutput)
		return true;

	ResetRayRegeneration();
	if (rayRegenerationContext || rayRegenerationFailureLatched)
		return false;
	auto& sc = globals::features::upscaling.dx12SwapChain;
	try {
		rrSharedSignal = new WrappedResource(SharedDesc(signal, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::Signal");
		rrSharedDepth = new WrappedResource(SharedDesc(depth, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::Depth");
		rrSharedMotion = new WrappedResource(SharedDesc(motion, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::Motion");
		rrSharedNormal = new WrappedResource(SharedDesc(normal, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::NormalRoughness");
		rrSharedSpecularAlbedo = new WrappedResource(SharedDesc(specAlbedo, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::SpecularAlbedo");
		rrSharedDiffuseAlbedo = new WrappedResource(SharedDesc(diffAlbedo, width, height, D3D11_BIND_SHADER_RESOURCE), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::DiffuseAlbedo");
		rrSharedOutput = new WrappedResource(SharedDesc(output, width, height, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS), sc.d3d11Device.get(), sc.d3d12Device.get(), "RayRegeneration::Output");

		ffxCreateContextDescDenoiser create{};
		create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
		create.version = FFX_DENOISER_VERSION;
		create.maxRenderSize = { width, height };
		create.signalFlags = FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR;
		create.checkerboardSignalFlags = 0;
		create.flags = 0;
		ffx::CreateBackendDX12Desc backend{};
		backend.device = sc.d3d12Device.get();
		create.header.pNext = &backend.header;
		if (ffxModule.CreateContext(&rayRegenerationContext, &create.header, nullptr) != FFX_API_RETURN_OK || !rayRegenerationContext) {
			logger::error("[RayRegeneration] FidelityFX Denoiser 1.2 context creation failed; disabling RR for this session");
			rayRegenerationFailureLatched = true;
			ResetRayRegeneration();
			return false;
		}
		rayRegenerationWidth = width;
		rayRegenerationHeight = height;
		logger::info("[RayRegeneration] FidelityFX Denoiser 1.2 context created at {}x{}", width, height);
		return true;
	} catch (const std::exception& e) {
		logger::error("[RayRegeneration] Resource/context setup failed: {}", e.what());
		ResetRayRegeneration();
		return false;
	}
}

/** @brief Reconstructs indirect specular and copies the synchronized result into the borrowed D3D11 output. */
bool FidelityFX::DispatchRayRegeneration(ID3D11Texture2D* signal, ID3D11Texture2D* depth, ID3D11Texture2D* motion,
	ID3D11Texture2D* normal, ID3D11Texture2D* specAlbedo, ID3D11Texture2D* diffAlbedo, ID3D11Texture2D* output,
	uint32_t width, uint32_t height, bool resetHistory)
{
	CS_GPU_PASS("Upscaling::RayRegenerationRuntimeDispatch");
	if (!EnsureRayRegenerationResources(signal, depth, motion, normal, specAlbedo, diffAlbedo, output, width, height))
		return false;
	auto& sc = globals::features::upscaling.dx12SwapChain;
	auto* cc = AcquireRuntimeCommandContext(false);
	if (!cc)
		return false;

	D3D11_BOX box{ 0, 0, 0, width, height, 1 };
	auto copy11 = [&](ID3D11Texture2D* src, WrappedResource* dst) { sc.d3d11Context->CopySubresourceRegion(dst->resource11, 0, 0, 0, 0, src, 0, &box); };
	bool commandListRecording = false;
	try {
		copy11(signal, rrSharedSignal);
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
		ID3D12Resource* reads[] = { rrSharedSignal->resource.get(), rrSharedDepth->resource.get(), rrSharedMotion->resource.get(), rrSharedNormal->resource.get(), rrSharedSpecularAlbedo->resource.get(), rrSharedDiffuseAlbedo->resource.get() };
		std::vector<D3D12_RESOURCE_BARRIER> begin;
		for (auto* r : reads) begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		begin.push_back(CD3DX12_RESOURCE_BARRIER::Transition(rrSharedOutput->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
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
		const auto& jitter = globals::features::upscaling.jitter;
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
		spec.signal.checkerboardOrigin = 0;
		common.header.pNext = &spec.header;
		bool dispatchFaulted = false;
		const ffxReturnCode_t dispatchResult = DispatchDenoiserProtected(&rayRegenerationContext, &common.header, dispatchFaulted);
		if (dispatchFaulted || dispatchResult != FFX_API_RETURN_OK) {
			if (rrDiagnosticsEnabled)
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

		if (rrDiagnosticsEnabled) {
			++rayRegenerationSuccessfulDispatches;

			if (resetHistory)
				++rayRegenerationHistoryResets;
		}

		return true;
	} catch (const std::exception& e) {
		if (commandListRecording) {
			(void)cc->commandList->Close();
			commandListRecording = false;
		}
		if (rrDiagnosticsEnabled)
			++rayRegenerationFailedDispatches;
		rayRegenerationFailureLatched = true;
		logger::error("[RayRegeneration] Dispatch failed: {}", e.what());
		return false;
	}
}


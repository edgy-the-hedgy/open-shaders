#include "NativeSceneComposition.h"
#include "Globals.h"
#include "GpuPass.h"
#include "NativeSceneOwnership.h"

namespace DX12Native
{
	ID3D12Resource* NativeSceneComposition::ResolveShadow(Frame& frame, const NativeShadowLayer& shadow, std::array<uint32_t, 2> size, uint64_t budget)
	{
		CS_GPU_PASS("DX12NativeScene::ShadowVisibilityResolve");
		frame.shadowPreviousSlot = uint32_t(frames.size());
		if (!shadow.temporal && shadow.traceResolution == 0) return shadow.gpu.first;
		try {
			if (!shadowResolve.Initialize(backend.d3d12Device.get(), "Data/Shaders/RTDirectLighting/nativeShadowResolve.dxil"))
				throw std::runtime_error(shadowResolve.Failure());
			if (frame.shadowHistory && (frame.shadowHistory->GetDesc().Width != size[0] || frame.shadowHistory->GetDesc().Height != size[1]))
				frame.shadowHistory = nullptr;
			if (!frame.shadowHistory) {
				const auto bytes = NativeTextureBytes(backend.d3d12Device.get(), size, DXGI_FORMAT_R32G32B32A32_FLOAT);
				if (!NativeSceneBudgetAllows(ResidentBytes(), bytes, budget))
					throw std::runtime_error("Shadow resolve exceeds composition budget; using genuine current samples only");
				frame.shadowHistory = CreateNativeTexture(backend.d3d12Device.get(), size, DXGI_FORMAT_R32G32B32A32_FLOAT, L"DX12native-shadowmask::ResolvedVisibility");
			}
			const auto number = globals::game::graphicsState->GetFrameCount();
			Frame* previous = shadowHistorySlot < frames.size() ? &frames[shadowHistorySlot] : nullptr;
			const bool history = shadow.temporal && previous && previous != &frame && previous->shadowHistory &&
				NativeShadowHistoryCompatible(number, previous->shadowFrame, shadow.sceneRevision, previous->shadowRevision, shadow.policy, previous->shadowPolicy) &&
				previous->shadowHistory->GetDesc().Width == size[0] &&
				previous->shadowHistory->GetDesc().Height == size[1];
			const auto previousOrigin = history ? previous->shadowOrigin : shadow.origin;
			const auto bits = [](float value) { return std::bit_cast<uint32_t>(value); };
			const std::array<uint32_t, 12> constants{ size[0], size[1], uint32_t(history), shadow.traceResolution,
				bits(shadow.origin[0]), bits(shadow.origin[1]), bits(shadow.origin[2]), bits(shadow.historyWeight),
				bits(previousOrigin[0]), bits(previousOrigin[1]), bits(previousOrigin[2]), bits(0.018f) };
			const std::array reads{ shadow.gpu.first, shadow.gpu.second, history ? previous->shadowHistory.get() : nullptr };
			const std::array writes{ frame.shadowHistory.get() };
			frame.shadowBindings = shadowResolve.RetainBindings();
			shadowResolve.Dispatch(backend.d3d12Device.get(), frame.commands.get(), uint32_t(&frame - frames.data()),
				reads, writes, constants, history ? previous->camera->GetGPUVirtualAddress() : frame.camera->GetGPUVirtualAddress(), size);
			frame.shadowOrigin = shadow.origin;
			frame.shadowRevision = shadow.sceneRevision;
			frame.shadowPolicy = shadow.policy;
			frame.shadowFrame = number;
			if (history) frame.shadowPreviousSlot = shadowHistorySlot;
			shadowTemporalFailure.clear();
			return frame.shadowHistory.get();
		} catch (const std::exception& error) {
			shadowTemporalFailure = error.what();
			return shadow.gpu.first;
		}
	}
}

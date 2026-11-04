#include "Deferred.h"
#include "Globals.h"
#include "GpuPass.h"
#include "NativeSceneComposition.h"
#include "NativeSceneOwnership.h"
#include "State.h"
#include "Util.h"

namespace DX12Native
{
	void NativeSceneComposition::CaptureSource(const GraphicsBackend& graphics, ID3D11ShaderResourceView* diffuse, ID3D11ShaderResourceView* specular,
		const std::array<uint32_t, 2>& size, uint32_t frameNumber, uint64_t budget)
	{
		CS_GPU_PASS("DX12NativeScene::ShadowedRasterInput");
		sourceFrame = UINT32_MAX;
		sourceValid = false;
		if (!diffuse || !specular || !size[0] || !size[1])
			return;
		if (!graphics.d3d12Device || !graphics.commandQueue || !graphics.d3d11Device || !graphics.d3d11Context) {
			failure = "Native scene handoff requires both graphics devices and their queues";
			return;
		}
		try {
			if (completion.fence12 && (backend.d3d12Device != graphics.d3d12Device || backend.d3d11Device != graphics.d3d11Device || backend.commandQueue != graphics.commandQueue))
				throw std::runtime_error("Native scene backend changed; recreate the graphics session");
			if (!completion.fence12) {
				backend = graphics;
				producer.Create(backend.d3d12Device.get(), backend.d3d11Device.get(), "DX12NativeScene::SceneHandoff");
				completion.Create(backend.d3d12Device.get(), backend.d3d11Device.get(), "DX12NativeScene::FinalSceneCompletion");
			}
			auto* context = backend.d3d11Context.get();
			auto available = std::find_if(initialSources.begin(), initialSources.end(), [&](const auto& item) {
				const auto index = size_t(&item - initialSources.data());
				return !item || NativeSceneReusable(item.use_count() != 1, producer.fence12->GetCompletedValue(), sourceReady[index], 0, 0);
			});
			if (available == initialSources.end())
				throw std::runtime_error("Both immutable scene-input slots are still in GPU use");
			sourceSlot = uint32_t(available - initialSources.begin());
			auto& initialRadiance = *available;
			if (!captureSource) {
				captureSource.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
					L"Data\\Shaders\\RedstoneGI\\nativeSource.cs.hlsl", {}, "cs_5_0")));
				if (!captureSource)
					throw std::runtime_error("Native pre-GI snapshot shader unavailable");
			}
			if (!initialRadiance || initialRadiance->resource->GetDesc().Width != size[0] || initialRadiance->resource->GetDesc().Height != size[1]) {
				initialRadiance.reset();
				D3D11_TEXTURE2D_DESC desc{};
				desc.Width = size[0];
				desc.Height = size[1];
				desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
				desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				const auto bytes = NativeTextureBytes(backend.d3d12Device.get(), size, desc.Format);
				if (!NativeSceneBudgetAllows(ResidentBytes(), bytes, budget))
					TrimIdleFrames();
				if (!NativeSceneBudgetAllows(ResidentBytes(), bytes, budget))
					throw std::runtime_error("Shared native scene input exceeds its memory budget");
				initialRadiance = std::make_shared<WrappedResource>(desc, backend.d3d11Device.get(), backend.d3d12Device.get(), "DX12NativeScene::InitiallyShadowedSceneInput");
				sourceSize = size;
			}
			if (!sourceSettings)
				sourceSettings = std::make_unique<ConstantBuffer>(ConstantBufferDesc<float4>(), "RedstoneGI::InitialSourceSettings");
			sourceSettings->Update(std::array<uint32_t, 4>{ size[0], size[1], 0, 0 });
			winrt::com_ptr<ID3D11ComputeShader> previousShader;
			std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> previousReads;
			winrt::com_ptr<ID3D11UnorderedAccessView> previousWrite;
			std::array<winrt::com_ptr<ID3D11Buffer>, 4> previousBuffers;
			constexpr std::array slots{ 5u, 6u, 12u, 13u };
			context->CSGetShader(previousShader.put(), nullptr, nullptr);
			for (uint32_t i = 0; i < 2; ++i) context->CSGetShaderResources(i, 1, previousReads[i].put());
			context->CSGetUnorderedAccessViews(0, 1, previousWrite.put());
			for (uint32_t i = 0; i < 4; ++i) context->CSGetConstantBuffers(slots[i], 1, previousBuffers[i].put());
			globals::state->BindSharedDataCS(context);
			auto* cb = sourceSettings->CB();
			context->CSSetConstantBuffers(13, 1, &cb);
			ID3D11ShaderResourceView* reads[]{ diffuse, specular };
			auto* write = initialRadiance->uav;
			context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
			context->CSSetShaderResources(0, 2, reads);
			context->CSSetShader(captureSource.get(), nullptr, 0);
			context->Dispatch((size[0] + 7u) / 8u, (size[1] + 7u) / 8u, 1);
			write = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
			for (uint32_t i = 0; i < 2; ++i) {
				auto* prior = previousReads[i].get();
				context->CSSetShaderResources(i, 1, &prior);
			}
			write = previousWrite.get();
			context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
			for (uint32_t i = 0; i < 4; ++i) {
				auto* prior = previousBuffers[i].get();
				context->CSSetConstantBuffers(slots[i], 1, &prior);
			}
			context->CSSetShader(previousShader.get(), nullptr, 0);
			const auto ready = producer.Next();
			DX::ThrowIfFailed(context->Signal(producer.fence11.get(), ready));
			context->Flush();
			DX::ThrowIfFailed(backend.commandQueue->Wait(producer.fence12.get(), ready));
			sourceReady[sourceSlot] = ready;
			sourceSize = size;
			sourceFrame = frameNumber;
			sourceValid = true;
			++sourceHandoffs;
			sourceSelectedProvider = globals::deferred->indirectLighting ? globals::deferred->indirectLighting->GetName() : "none";
		} catch (const std::exception& error) {
			failure = error.what();
		}
	}

	std::optional<NativeSceneOutput> NativeSceneComposition::GetSceneInput(uint32_t frameNumber, const std::array<uint32_t, 2>& size) const
	{
		if (!sourceValid || sourceFrame != frameNumber || sourceSize != size || !initialSources[sourceSlot])
			return std::nullopt;
		NativeSceneOutput output{ initialSources[sourceSlot]->resource.get(), nullptr, producer.fence12.get(), sourceReady[sourceSlot] };
		output.sourceOwner = initialSources[sourceSlot];
		return output;
	}

}

#include "RasterCapture.h"
#include "Features/Upscaling.h"
#include "GpuPass.h"
#include "Utils/D3D.h"
#include <algorithm>
#include <ranges>

namespace DX12
{
	RasterCapture::~RasterCapture()
	{
		current.reset();
		for (auto& slot : slots) {
			if (!slot) continue;
			const auto completed = producer.fence12 ? producer.fence12->GetCompletedValue() : 0;
			const bool pending = completed == UINT64_MAX || !slot->readyValue ||
				completed < slot->readyValue ||
				std::ranges::any_of(slot->consumers, [](const auto& consumer) {
					const auto done = consumer.first->GetCompletedValue();
					return done == UINT64_MAX || done < consumer.second;
				});
			if (pending) (void)new std::shared_ptr<RasterFrame>(std::move(slot));
		}
	}

	void RasterCapture::Capture(ID3D11ShaderResourceView* depth,
		const globals::FrameBuffer& camera, uint32_t frame)
	{
		CS_GPU_PASS("DX12NativeScene::CaptureDepth");
		current.reset();
		auto& backend = globals::features::upscaling.dx12SwapChain;
		try {
			if (!depth || !backend.d3d11Device || !backend.d3d11Context ||
				!backend.d3d12Device || !backend.commandQueue)
				return;
			if (!producer.fence11)
				producer.Create(backend.d3d12Device.get(), backend.d3d11Device.get(),
					"DX12NativeScene::RasterReady");
			winrt::com_ptr<ID3D11Resource> resource;
			depth->GetResource(resource.put());
			auto texture = resource.try_as<ID3D11Texture2D>();
			D3D11_TEXTURE2D_DESC source{};
			if (!texture) return;
			texture->GetDesc(&source);
			if (!source.Width || !source.Height || source.ArraySize != 1 || source.SampleDesc.Count != 1)
				return;
			if (!depthShader)
				depthShader.attach(static_cast<ID3D11ComputeShader*>(
					Util::CompileShader(L"Data\\Shaders\\DX12NativeComposite\\CaptureDepth.cs.hlsl", {}, "cs_5_0")));
			if (!depthShader) return;
			const auto completed = producer.fence12->GetCompletedValue();
			auto slot = std::find_if(slots.begin(), slots.end(), [&](const auto& entry) {
				if (!entry) return true;
				if (entry.use_count() != 1 || completed == UINT64_MAX) return false;
			if (!entry->readyValue) return true; // Reuse an unpublished slot after a failed capture.
			if (completed < entry->readyValue) return false;
				return std::ranges::all_of(entry->consumers, [](const auto& consumer) {
					const auto done = consumer.first->GetCompletedValue();
					return done != UINT64_MAX && done >= consumer.second;
				});
			});
			if (slot == slots.end()) return; // All slots in use; do not block the CPU.
			auto& captured = *slot;
			if (!captured || captured->width != source.Width || captured->height != source.Height) {
				D3D11_TEXTURE2D_DESC desc = source;
				desc.Format = DXGI_FORMAT_R32_FLOAT;
				desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				desc.CPUAccessFlags = desc.MiscFlags = 0;
				auto next = std::make_shared<RasterFrame>();
				next->width = source.Width;
				next->height = source.Height;
				next->depth = std::make_unique<WrappedResource>(desc,
					backend.d3d11Device.get(), backend.d3d12Device.get(),
					"DX12NativeScene::RasterDepth");
				captured = std::move(next);
			}
			captured->consumers.clear();
			auto* context = backend.d3d11Context.get();
			winrt::com_ptr<ID3D11ComputeShader> oldShader;
			winrt::com_ptr<ID3D11ShaderResourceView> oldRead;
			winrt::com_ptr<ID3D11UnorderedAccessView> oldWrite;
			context->CSGetShader(oldShader.put(), nullptr, nullptr);
			context->CSGetShaderResources(0, 1, oldRead.put());
			context->CSGetUnorderedAccessViews(0, 1, oldWrite.put());
			auto* write = captured->depth->uav;
			context->CSSetShaderResources(0, 1, &depth);
			context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
			context->CSSetShader(depthShader.get(), nullptr, 0);
			context->Dispatch((source.Width + 7) / 8, (source.Height + 7) / 8, 1);
			write = oldWrite.get();
			context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
			auto* read = oldRead.get();
			context->CSSetShaderResources(0, 1, &read);
			context->CSSetShader(oldShader.get(), nullptr, 0);
			const uint64_t ready = producer.Next();
			DX::ThrowIfFailed(context->Signal(producer.fence11.get(), ready));
			context->Flush();
			DX::ThrowIfFailed(backend.commandQueue->Wait(producer.fence12.get(), ready));
			captured->camera = camera;
			captured->frame = frame;
			captured->readyFence = producer.fence12;
			captured->readyValue = ready;
			current = captured;
			status.clear();
		} catch (const std::exception& e) {
			status = e.what();
		}
	}
	std::shared_ptr<const RasterFrame> RasterCapture::Get(uint32_t frame) const
	{
		return current && current->frame == frame ? current : nullptr;
	}
	void RasterCapture::Reset() { current.reset(); }
}

#include "DX12NativeEnvironment.h"
#include "DX12GpuRetirement.h"
#include "GpuPass.h"

#include <algorithm>
#include <directx/d3dx12.h>
#include <stdexcept>

namespace DX12Native
{
	bool EnvironmentCube::IsReadyFor(const EnvironmentStamp& requested) const
	{
		return stamp.SameEnvironment(requested) && revision && texture && texture->resource &&
			width && mipCount && readyFence &&
			MaterialFrameContract::ValidProducerSignal(readyValue, readyFence->GetCompletedValue());
	}

	NativeEnvironment::~NativeEnvironment()
	{
		current.reset();
		for (auto& pool : pools) {
			pool.current.reset();
			for (auto& slot : pool.slots)
				if (slot && !GpuFenceRetired(slot->readyFence.get(), slot->readyValue))
					RetireWhenGpuDone(std::move(slot), owner.d3d12Device, [](const EnvironmentCube& cube) {
						return GpuFenceRetired(cube.readyFence.get(), cube.readyValue);
					});
		}
	}

	uint64_t NativeEnvironment::AllocationBytes() const
	{
		uint64_t bytes = 0;
		for (const auto& pool : pools)
			for (const auto& slot : pool.slots)
				if (slot) bytes += slot->allocationBytes;
		return bytes;
	}

	void NativeEnvironment::Poll(bool releaseUnused)
	{
		for (auto& pool : pools) {
			for (auto& slot : pool.slots) {
				if (!slot) continue;
				if (slot->readyValue == UINT64_MAX && producer.fence11 && owner.d3d11Context &&
					producer.fence12->GetCompletedValue() != UINT64_MAX) {
					const auto rescue = producer.Next();
					if (SUCCEEDED(owner.d3d11Context->Signal(producer.fence11.get(), rescue))) {
						owner.d3d11Context->Flush();
						slot->readyValue = rescue;
					}
				}
				if (releaseUnused && slot.use_count() == 1 &&
					GpuFenceRetired(slot->readyFence.get(), slot->readyValue))
					slot.reset();
			}
		}
	}

	void NativeEnvironment::Reset()
	{
		current.reset();
		for (auto& pool : pools) pool.current.reset();
		Poll(true);
		failure.clear();
	}

	std::shared_ptr<const EnvironmentCube> NativeEnvironment::CaptureCube(CubePool& pool,
		const EnvironmentSource& source, const EnvironmentStamp& requested, const char* name)
	{
		if (!source.IsValidFor(requested)) {
			pool.current.reset();
			return {};
		}
		if (pool.current && pool.current->IsReadyFor(requested) && pool.current->revision == source.revision)
			return pool.current;

		D3D11_TEXTURE2D_DESC desc{};
		source.texture->GetDesc(&desc);
		winrt::com_ptr<ID3D11Device> sourceDevice;
		source.texture->GetDevice(sourceDevice.put());
		if (sourceDevice.get() != static_cast<ID3D11Device*>(owner.d3d11Device.get()) ||
			desc.ArraySize != 6 || desc.Width != desc.Height || !desc.Width || !desc.MipLevels ||
			desc.SampleDesc.Count != 1 || !(desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE))
			throw std::runtime_error("Environment source is not a compatible cube");

		auto* device = owner.d3d12Device.get();
		D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ desc.Format };
		if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
			!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURECUBE) ||
			!(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))
			throw std::runtime_error("Environment cube format unsupported by DX12");

		auto selected = std::find_if(pool.slots.begin(), pool.slots.end(), [&](const auto& slot) {
			return !slot || (slot.use_count() == 1 && GpuFenceRetired(slot->readyFence.get(), slot->readyValue));
		});
		if (selected == pool.slots.end())
			return pool.current && pool.current->IsReadyFor(requested) ? pool.current : nullptr;
		auto& slot = *selected;
		if (!slot || slot->format != desc.Format || slot->width != desc.Width || slot->mipCount != desc.MipLevels) {
			const auto resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(desc.Format, desc.Width, desc.Height,
				static_cast<UINT16>(desc.ArraySize), static_cast<UINT16>(desc.MipLevels));
			const uint64_t allocation = device->GetResourceAllocationInfo(0, 1, &resourceDesc).SizeInBytes;
			const uint64_t tracked = AllocationBytes();
			const auto budget = videoBudget.Query(device, tracked);
			if (!allocation || allocation == UINT64_MAX || !budget.dxgiAvailable ||
				allocation > budget.effectiveBytes || tracked > budget.effectiveBytes - allocation)
				throw std::runtime_error("Environment allocation deferred by DXGI budget");
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
			auto replacement = std::make_shared<EnvironmentCube>();
			replacement->texture = std::make_unique<WrappedResource>(desc, owner.d3d11Device.get(), device, name);
			replacement->allocationBytes = allocation;
			replacement->width = desc.Width;
			replacement->mipCount = desc.MipLevels;
			replacement->format = desc.Format;
			slot = std::move(replacement);
		}

		slot->stamp = source.stamp;
		slot->revision = source.revision;
		slot->readyFence = producer.fence12;
		slot->readyValue = UINT64_MAX;
		auto* context = owner.d3d11Context.get();
		context->CopyResource(slot->texture->resource11, source.texture);
		const auto value = producer.Next();
		if (FAILED(context->Signal(producer.fence11.get(), value))) {
			const auto rescue = producer.Next();
			if (SUCCEEDED(context->Signal(producer.fence11.get(), rescue))) {
				context->Flush();
				slot->readyValue = rescue;
			}
			throw std::runtime_error("Environment producer signal failed");
		}
		slot->readyValue = value;
		context->Flush();
		pool.current = slot;
		return slot;
	}

	void NativeEnvironment::Capture(const GraphicsBackend& backend, const EnvironmentStamp& requested,
		const EnvironmentSource& base, const EnvironmentSource& reflection, const EnvironmentSource& sky)
	{
		CS_GPU_PASS("NativeLighting::EnvironmentPublication");
		if (!requested.epoch || !requested.scope) {
			Reset();
			return;
		}
		Poll();
		try {
			if (!producer.fence12) {
				owner = backend;
				producer.Create(owner.d3d12Device.get(), owner.d3d11Device.get(), "NativeLighting::EnvironmentReady");
			}
			if (owner.d3d12Device.get() != backend.d3d12Device.get() ||
				owner.d3d11Device.get() != backend.d3d11Device.get() ||
				owner.commandQueue.get() != backend.commandQueue.get() ||
				producer.fence12->GetCompletedValue() == UINT64_MAX)
				throw std::runtime_error("Environment backend unavailable");

			const auto capture = [&](size_t index, const EnvironmentSource& source, const char* name) -> std::shared_ptr<const EnvironmentCube> {
				try {
					return CaptureCube(pools[index], source, requested, name);
				} catch (const std::exception& e) {
					failure = e.what();
					auto& old = pools[index].current;
					return old && old->IsReadyFor(requested) ? old : nullptr;
				}
			};
			failure.clear();
			const auto baseCube = capture(0, base, "NativeLighting::BaseEnvironment");
			const auto reflectionCube = capture(1, reflection, "NativeLighting::ReflectionEnvironment");
			const auto skyCube = capture(2, sky, "NativeLighting::SkyRadiance");
			if (!baseCube) {
				current.reset();
				return;
			}
			if (current && current->stamp.SameEnvironment(requested) && current->base == baseCube &&
				current->reflection == reflectionCube && current->sky == skyCube)
				return;

			auto frame = std::make_shared<EnvironmentFrame>();
			frame->stamp = requested;
			frame->revision = ++publications;
			frame->base = baseCube;
			frame->reflection = reflectionCube;
			frame->sky = skyCube;
			frame->producerFence = producer.fence12;
			frame->producerValue = std::max({ baseCube->readyValue, reflectionCube ? reflectionCube->readyValue : 0,
				skyCube ? skyCube->readyValue : 0 });
			current = std::move(frame);
		} catch (const std::exception& e) {
			current.reset();
			failure = e.what();
		}
	}
}

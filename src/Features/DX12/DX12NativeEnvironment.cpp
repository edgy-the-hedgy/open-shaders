#include "DX12NativeEnvironment.h"
#include "DX12MaterialFrameContract.h"
#include "DX12GpuRetirement.h"
#include "GpuPass.h"

#include <algorithm>
#include <limits>
#include <directx/d3dx12.h>
#include <stdexcept>

namespace DX12Native
{
	bool EnvironmentCube::IsReadyFor(const EnvironmentStamp& requested) const
	{
		return stamp.SameEnvironment(requested) && revision && resource &&
			width && mipCount && readyFence &&
			MaterialFrameContract::ValidProducerSignal(readyValue, readyFence->GetCompletedValue());
	}

	bool EnvironmentCube::Retired() const
    {
        return GpuFenceRetired(sourceFence.get(), sourceValue) && GpuFenceRetired(readyFence.get(), readyValue);
    }

	NativeEnvironment::~NativeEnvironment()
	{
		current.reset();
		for (auto& pool : pools) {
			pool.current.reset();
			for (auto& slot : pool.slots) {
				if (!slot || slot->Retired())
					continue;
				std::vector<FenceDependency> dependencies{
					FencePoint(slot->sourceFence.get(), slot->sourceValue),
					FencePoint(slot->readyFence.get(), slot->readyValue)
				};
				RetireAfterFences(std::move(slot), owner.d3d12Device, std::move(dependencies));
			}
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
				if (slot->sourceValue == UINT64_MAX && producer.fence11 && owner.d3d11Context &&
					producer.fence12->GetCompletedValue() != UINT64_MAX) {
					const auto rescue = producer.Next();
					if (SUCCEEDED(owner.d3d11Context->Signal(producer.fence11.get(), rescue))) {
						owner.d3d11Context->Flush();
						slot->sourceValue = rescue;
					}
				}
                if (slot->readyValue == UINT64_MAX && copyFence && owner.commandQueue &&
                    copyFence->GetCompletedValue() != UINT64_MAX) {
                    const uint64_t rescue = nextCopyValue + 1;
                    if (SUCCEEDED(owner.commandQueue->Signal(copyFence.get(), rescue))) {
                        nextCopyValue = rescue; slot->readyValue = rescue;
                    }
                }
				if (releaseUnused && slot.use_count() == 1 &&
					slot->Retired())
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
			return !slot || (slot.use_count() == 1 && slot->Retired());
		});
		if (selected == pool.slots.end())
			return pool.current && pool.current->IsReadyFor(requested) ? pool.current : nullptr;
        auto& slot = *selected;
        // Single-mip 2D staging: pack all six faces in each mip row. No CPU readback
        // or shared mipmapped array support is required.
        if (desc.Format != DXGI_FORMAT_R11G11B10_FLOAT && desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
            desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM &&
            desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
            throw std::runtime_error("Unsupported uncompressed environment staging format");
        if (desc.Width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION / 6 || desc.MipLevels > 15)
            throw std::runtime_error("Environment staging dimensions unsupported");
        UINT stagingHeight = 0;
        for (UINT mip = 0; mip < desc.MipLevels; ++mip) stagingHeight += std::max(1u, desc.Width >> mip);
        if (stagingHeight > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
            throw std::runtime_error("Environment staging height unsupported");
        const UINT stagingWidth = 6 * desc.Width;
        if (!slot || slot->format != desc.Format || slot->width != desc.Width || slot->mipCount != desc.MipLevels) {
            const auto cubeDesc = CD3DX12_RESOURCE_DESC::Tex2D(desc.Format, desc.Width, desc.Height,
                static_cast<UINT16>(desc.ArraySize), static_cast<UINT16>(desc.MipLevels));
            const auto stagingDesc = CD3DX12_RESOURCE_DESC::Tex2D(desc.Format, stagingWidth, stagingHeight);
            const uint64_t cubeBytes = device->GetResourceAllocationInfo(0, 1, &cubeDesc).SizeInBytes;
            const uint64_t stagingBytes = device->GetResourceAllocationInfo(0, 1, &stagingDesc).SizeInBytes;
            if (!cubeBytes || !stagingBytes || cubeBytes == UINT64_MAX || stagingBytes == UINT64_MAX ||
                cubeBytes > UINT64_MAX - stagingBytes)
                throw std::runtime_error("Environment allocation unsupported");
            const uint64_t allocation = cubeBytes + stagingBytes;
            const uint64_t tracked = AllocationBytes();
            const auto budget = videoBudget.Query(device, tracked);
            if (!budget.localBudget || allocation > budget.effectiveBytes || tracked > budget.effectiveBytes - allocation)
                throw std::runtime_error("Environment allocation deferred by DXGI budget");
            D3D11_TEXTURE2D_DESC sharedDesc{};
            sharedDesc.Width = stagingWidth; sharedDesc.Height = stagingHeight;
            sharedDesc.MipLevels = 1; sharedDesc.ArraySize = 1; sharedDesc.Format = desc.Format;
            sharedDesc.SampleDesc.Count = 1; sharedDesc.Usage = D3D11_USAGE_DEFAULT;
            sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            auto replacement = std::make_shared<EnvironmentCube>();
            replacement->staging = std::make_unique<WrappedResource>(sharedDesc, owner.d3d11Device.get(), device, name);
            const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &cubeDesc, D3D12_RESOURCE_STATE_COMMON,
                nullptr, IID_PPV_ARGS(replacement->resource.put()))) ||
                FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(replacement->allocator.put()))) ||
                FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, replacement->allocator.get(), nullptr,
                    IID_PPV_ARGS(replacement->commands.put()))) ||
                FAILED(replacement->commands->Close()))
                throw std::runtime_error("Environment DX12 cube allocation failed");
            replacement->allocationBytes = allocation;
            replacement->width = desc.Width; replacement->mipCount = desc.MipLevels; replacement->format = desc.Format;
            slot = std::move(replacement);
        }

        slot->stamp = source.stamp;
        slot->revision = source.revision;
        slot->sourceFence = producer.fence12; slot->sourceValue = 0;
        slot->readyFence = copyFence; slot->readyValue = 0;
        if (FAILED(slot->allocator->Reset()) || FAILED(slot->commands->Reset(slot->allocator.get(), nullptr))) {
            slot.reset(); throw std::runtime_error("Environment copy command reset failed");
        }
        auto* command = slot->commands.get();
        auto* staging = slot->staging->resource.get();
        auto* cube = slot->resource.get();
        const D3D12_RESOURCE_BARRIER begin[]{
            CD3DX12_RESOURCE_BARRIER::Transition(staging, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(cube, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)
        };
        command->ResourceBarrier(2, begin);
        auto* context = owner.d3d11Context.get();
        UINT y = 0;
        slot->sourceValue = UINT64_MAX;
        for (UINT mip = 0; mip < desc.MipLevels; ++mip) {
            const UINT size = std::max(1u, desc.Width >> mip);
            for (UINT face = 0; face < 6; ++face) {
                const UINT sub = D3D11CalcSubresource(mip, face, desc.MipLevels);
                const UINT x = face * size;
                context->CopySubresourceRegion(slot->staging->resource11, 0, x, y, 0, source.texture, sub, nullptr);
                const CD3DX12_TEXTURE_COPY_LOCATION src(staging, 0);
                const CD3DX12_TEXTURE_COPY_LOCATION dst(cube, sub);
                const D3D12_BOX box{ x, y, 0, x + size, y + size, 1 };
                command->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
            }
            y += size;
        }
        const D3D12_RESOURCE_BARRIER finish[]{
            CD3DX12_RESOURCE_BARRIER::Transition(staging, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
            CD3DX12_RESOURCE_BARRIER::Transition(cube, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)
        };
        command->ResourceBarrier(2, finish);
        const auto value = producer.Next();
        if (FAILED(context->Signal(producer.fence11.get(), value))) {
            const auto rescue = producer.Next();
            if (SUCCEEDED(context->Signal(producer.fence11.get(), rescue))) {
                context->Flush(); slot->sourceValue = rescue;
            }
            command->Close();
            throw std::runtime_error("Environment staging signal failed");
        }
        slot->sourceValue = value;
        context->Flush();
        if (FAILED(command->Close()) || FAILED(owner.commandQueue->Wait(producer.fence12.get(), value)))
            throw std::runtime_error("Environment copy scheduling failed");
        ID3D12CommandList* lists[]{ command };
        owner.commandQueue->ExecuteCommandLists(1, lists);
        slot->readyValue = UINT64_MAX;
        const uint64_t copyValue = nextCopyValue + 1;
        if (FAILED(owner.commandQueue->Signal(copyFence.get(), copyValue)))
            throw std::runtime_error("Environment DX12 copy signal failed");
        nextCopyValue = copyValue;
        slot->readyValue = copyValue;
		pool.current = slot;
		return slot;
	}

	void NativeEnvironment::Capture(const GraphicsBackend& backend, const EnvironmentStamp& requested,
		const EnvironmentSource& base, const EnvironmentSource& reflection, const EnvironmentSource& sky, bool allowMissingBase)
	{
		CS_GPU_PASS("NativeLighting::EnvironmentPublication");
		if (!requested.epoch || !requested.scope) {
			Reset();
			return;
		}
		Poll();
		try {
			if (!producer.fence12 || !copyFence) {
				owner = backend;
				producer.Create(owner.d3d12Device.get(), owner.d3d11Device.get(), "NativeLighting::EnvironmentReady");
				if (FAILED(owner.d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(copyFence.put()))))
					throw std::runtime_error("Environment copy fence creation failed");
			}
			if (owner.d3d12Device.get() != backend.d3d12Device.get() ||
				owner.d3d11Device.get() != backend.d3d11Device.get() ||
				owner.commandQueue.get() != backend.commandQueue.get() ||
				(producer.fence12->GetCompletedValue() == UINT64_MAX || copyFence->GetCompletedValue() == UINT64_MAX))
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
			if (!baseCube && !allowMissingBase) {
				current.reset();
				return;
			}
			if (current && current->stamp.SameEnvironment(requested) &&
				(!allowMissingBase || current->stamp.engineFrame == requested.engineFrame) && current->base == baseCube &&
				current->reflection == reflectionCube && current->sky == skyCube)
				return;

			auto frame = std::make_shared<EnvironmentFrame>();
			frame->stamp = requested;
			frame->revision = ++publications;
			frame->base = baseCube;
			frame->reflection = reflectionCube;
			frame->sky = skyCube;
			frame->producerFence = copyFence;
			frame->producerValue = std::max({ baseCube ? baseCube->readyValue : 0, reflectionCube ? reflectionCube->readyValue : 0,
				skyCube ? skyCube->readyValue : 0 });
			current = std::move(frame);
		} catch (const std::exception& e) {
			current.reset();
			failure = e.what();
		}
	}
}

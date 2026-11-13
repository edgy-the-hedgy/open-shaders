#pragma once

#include <algorithm>
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <winrt/base.h>

/** @brief Queries adapter-local DXGI headroom before allocating native frame textures. */
struct GraphicsMemoryBudget
{
	static constexpr uint64_t kReservePercent = 10;

	/** @brief Returns the currently safe total for this feature's tracked allocations. */
	static uint64_t Evaluate(uint64_t a_budget, uint64_t a_usage, uint64_t a_tracked)
	{
		if (!a_budget)
			return 0;
		const auto headroom = a_budget > a_usage ? a_budget - a_usage : 0;
		const auto reserve = (a_budget / 100) * kReservePercent + ((a_budget % 100) * kReservePercent) / 100;
		const auto available = headroom > reserve ? headroom - reserve : 0;
		return a_tracked > UINT64_MAX - available ? UINT64_MAX : a_tracked + available;
	}

	/** @brief Queries the native device's adapter, returning zero on unavailable telemetry. */
	uint64_t Query(ID3D12Device* a_device, uint64_t a_tracked)
	{
		if (!a_device)
			return 0;
		const auto luid = a_device->GetAdapterLuid();
		if (!adapter || luid.HighPart != adapterLuid.HighPart || luid.LowPart != adapterLuid.LowPart) {
			adapter = nullptr;
			winrt::com_ptr<IDXGIFactory4> factory;
			if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))) ||
				FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(adapter.put()))))
				return 0;
			adapterLuid = luid;
		}
		DXGI_QUERY_VIDEO_MEMORY_INFO info{};
		if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
			return 0;
		return Evaluate(info.Budget, info.CurrentUsage, a_tracked);
	}

private:
	winrt::com_ptr<IDXGIAdapter3> adapter;
	LUID adapterLuid{};
};

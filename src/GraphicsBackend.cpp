#include "GraphicsBackend.h"

#include <dxgi1_2.h>
#include <mutex>
#include <atomic>
#include <string_view>

namespace
{
	std::atomic<bool> diagnoseNativeDevice{ false };
	std::atomic<bool> nativeDeviceLost{ false };
	constexpr uint32_t kMaxBreadcrumbs = 64;
	constexpr uint32_t kMaxAllocations = 16;

	std::string DredName(const char* narrow, const wchar_t* wide)
	{
		if (wide && *wide) return winrt::to_string(std::wstring_view(wide));
		return narrow && *narrow ? narrow : "(unnamed)";
	}

	void ReportDred(ID3D12Device* device)
	{
		if (!device)
			return;
		winrt::com_ptr<ID3D12DeviceRemovedExtendedData> dred;
		if (FAILED(device->QueryInterface(IID_PPV_ARGS(dred.put())))) {
			logger::warn("[DX12 DRED] Device does not expose removed-device data");
			return;
		}
		D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
		if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
			uint32_t count = 0;
			for (auto* node = breadcrumbs.pHeadAutoBreadcrumbNode; node && count < kMaxBreadcrumbs; node = node->pNext) {
				const uint32_t completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0u;
				if (completed >= node->BreadcrumbCount)
					continue;
				const auto next = node->pCommandHistory ? uint32_t(node->pCommandHistory[completed]) : UINT32_MAX;
				const auto last = completed && node->pCommandHistory ? uint32_t(node->pCommandHistory[completed - 1]) : UINT32_MAX;
				logger::error("[DX12 DRED] Queue '{}' list '{}' completed {}/{} last op {} next op {}",
					DredName(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW),
					DredName(node->pCommandListDebugNameA, node->pCommandListDebugNameW),
					completed, node->BreadcrumbCount, last, next);
				++count;
			}
			logger::info("[DX12 DRED] Incomplete breadcrumb lists reported: {}", count);
		} else {
			logger::warn("[DX12 DRED] Breadcrumb output unavailable");
		}
		D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
		if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault))) {
			logger::error("[DX12 DRED] Page fault VA 0x{:016X}", fault.PageFaultVA);
			const auto report = [](const D3D12_DRED_ALLOCATION_NODE* node, const char* status) {
				for (uint32_t count = 0; node && count < kMaxAllocations; ++count, node = node->pNext)
					logger::error("[DX12 DRED] {} allocation '{}' type {}", status,
						DredName(node->ObjectNameA, node->ObjectNameW), uint32_t(node->AllocationType));
			};
			report(fault.pHeadExistingAllocationNode, "existing");
			report(fault.pHeadRecentFreedAllocationNode, "recently freed");
		} else {
			logger::warn("[DX12 DRED] Page fault output unavailable");
		}
	}
}

void GraphicsBackend::EnableNativeDeviceDiagnostics()
{
	if (diagnoseNativeDevice.exchange(true, std::memory_order_acq_rel))
		return;
	winrt::com_ptr<ID3D12DeviceRemovedExtendedDataSettings> settings;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(settings.put())))) {
		settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
		settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
		logger::info("[DX12 DRED] Full Native breadcrumbs and page fault reporting requested");
	} else {
		logger::warn("[DX12 DRED] Settings interface unavailable");
	}
}

bool GraphicsBackend::NativeDeviceLost()
{
	return nativeDeviceLost.load(std::memory_order_acquire);
}

bool GraphicsBackend::CheckNativeDeviceHealth(ID3D12Device* device12, ID3D11Device* device11)
{
	if (!diagnoseNativeDevice.load(std::memory_order_acquire))
		return true;
	if (NativeDeviceLost())
		return false;
	const auto reason12 = device12 ? device12->GetDeviceRemovedReason() : S_OK;
	const auto reason11 = device11 ? device11->GetDeviceRemovedReason() : S_OK;
	if (SUCCEEDED(reason12) && SUCCEEDED(reason11))
		return true;
	if (!nativeDeviceLost.exchange(true, std::memory_order_acq_rel)) {
		logger::critical("[DX12] Device loss confirmed: DX12 0x{:08X}, DX11 0x{:08X}. Full Native submissions stopped; restart required.",
			uint32_t(reason12), uint32_t(reason11));
		if (FAILED(reason12))
			ReportDred(device12);
	}
	return false;
}

HRESULT GraphicsBackend::GetSharedDevice(IDXGIAdapter* a_adapter, ID3D12Device** a_device)
{
	if (!a_adapter || !a_device)
		return E_INVALIDARG;
	if (NativeDeviceLost()) return DXGI_ERROR_DEVICE_REMOVED;
	static std::mutex mutex;
	static winrt::com_ptr<ID3D12Device> shared;
	std::scoped_lock lock(mutex);
	DXGI_ADAPTER_DESC adapterDesc{};
	const auto result = a_adapter->GetDesc(&adapterDesc);
	if (FAILED(result))
		return result;
	if (shared) {
		const auto removed = shared->GetDeviceRemovedReason();
		if (!CheckNativeDeviceHealth(shared.get())) return FAILED(removed) ? removed : DXGI_ERROR_DEVICE_REMOVED;
		if (FAILED(removed)) {
			// Drop our process-wide owner so a later Create can retry with a new device.
			// Other consumers release their own references independently.
			shared = nullptr;
			return removed;
		}
		const auto luid = shared->GetAdapterLuid();
		if (luid.HighPart != adapterDesc.AdapterLuid.HighPart || luid.LowPart != adapterDesc.AdapterLuid.LowPart)
			return DXGI_ERROR_UNSUPPORTED;
	} else {
		const auto created = D3D12CreateDevice(a_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(shared.put()));
		if (FAILED(created))
			return created;
	}
	shared.copy_to(a_device);
	return S_OK;
}

std::optional<GraphicsBackend> GraphicsBackend::Create(ID3D11Device* a_device, ID3D11DeviceContext* a_context, std::string& a_failure)
{
	GraphicsBackend backend;
	if (!a_device || !a_context) {
		a_failure = "DX11 device or context unavailable";
		return std::nullopt;
	}
	auto check = [&](HRESULT result, const char* operation) {
		if (FAILED(result))
			a_failure = std::format("{} failed: 0x{:08X}", operation, static_cast<uint32_t>(result));
		return SUCCEEDED(result);
	};
	winrt::com_ptr<IDXGIDevice> dxgiDevice;
	winrt::com_ptr<IDXGIAdapter> adapter;
	if (!check(a_device->QueryInterface(IID_PPV_ARGS(backend.d3d11Device.put())), "Query DX11 device") ||
		!check(a_context->QueryInterface(IID_PPV_ARGS(backend.d3d11Context.put())), "Query DX11 context") ||
		!check(a_device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())), "Query DXGI device") ||
		!check(dxgiDevice->GetAdapter(adapter.put()), "Get DX11 adapter") ||
		!check(GetSharedDevice(adapter.get(), backend.d3d12Device.put()), "Create DX12 device"))
		return std::nullopt;
	D3D12_COMMAND_QUEUE_DESC desc{};
	desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (!check(backend.d3d12Device->CreateCommandQueue(&desc, IID_PPV_ARGS(backend.commandQueue.put())), "Create DX12 queue"))
		return std::nullopt;
	backend.commandQueue->SetName(L"GraphicsBackend::CommandQueue");
	a_failure.clear();
	return backend;
}

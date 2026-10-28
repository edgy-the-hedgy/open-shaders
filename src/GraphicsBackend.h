#pragma once
#include <FidelityFX/api/include/ffx_api_loader.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

/** @brief Retains only the devices, queue and loader API of an existing graphics session. */
struct GraphicsBackend
{
	winrt::com_ptr<ID3D11Device5> d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext4> d3d11Context;
	winrt::com_ptr<ID3D12Device> d3d12Device;
	winrt::com_ptr<ID3D12CommandQueue> commandQueue;
	ffxFunctions fidelityFX{};
	float2 jitter{};
	bool fsrFrameGeneration = false;
	bool rayRegenerationHardware = false;
};

#pragma once
#include "NativeCompute.h"
#include <optional>
class WrappedResource;

namespace DX12Native
{
	/** @brief Borrows current-frame GPU contributions until the final scene consumer is submitted. */
	struct NativeSceneOutput
	{
		ID3D12Resource* first = nullptr;
		ID3D12Resource* second = nullptr;
		ID3D12Fence* ready = nullptr;
		uint64_t value = 0;
		float blend = 0;
		bool legacy = false;
		float legacyStrength = 1;
		std::shared_ptr<WrappedResource> sourceOwner;
	};
}

#pragma once

#include "RE/N/NiLight.h"
#include <cstdint>

namespace DX12Native
{
	// Shared source identity helpers used by existing static-shadow inventory and NativeLLF.
	void ReadFullLightOwner(const RE::NiLight* ni, uint32_t& owner, uint32_t& placed) noexcept;
	void ReadFullLightRestPosition(RE::NiLight* ni, uint32_t placedReference, float3& position) noexcept;
}

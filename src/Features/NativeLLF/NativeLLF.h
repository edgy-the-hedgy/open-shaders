#pragma once

#include "Features/DX12/DX12NativeDirectLighting.h"
#include "Features/DX12/FullNativeFeature.h"

#include <optional>
#include <vector>

namespace NativeLLF
{
	/** @brief Synchronous CPU light-input snapshot; GPU upload remains owned by DirectLightingTrace. */
	struct DirectInputFrame
	{
		uint32_t frame = 0, scope = 0;
		uint64_t geometryRevision = 0;
		std::vector<DX12Native::DirectPointLight> points;
		DX12Native::DirectDirectionalLight directional;
		bool available = false;
	};

	class Producer final : public DX12::FullNativeFeature
	{
	public:
		std::string GetName() override { return "Native LLF"; }
		std::string GetShortName() override { return "NativeLLF"; }
		void ResetParticleInput();
		void OnRuntimeDisabled() override { ResetParticleInput(); }

		// Transitional particle capture bridge until raw engine capture is independently routed.
		std::optional<DirectInputFrame> CollectDirectInputs(const DX12Native::TopLevel& scene, uint32_t frame, bool opaqueReady);
	};
}

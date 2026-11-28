#pragma once

#include "Features/DX12/FullNativeFeature.h"

#include "Features/DX12/DX12NativeIndirectLighting.h"
#include "Features/IBL/IBLSettingsInput.h"

namespace NativeIBL
{
	/** @brief Environment-only indirect lighting consumer; owns its DX12 resources. */
	class Producer final : public DX12::FullNativeFeature
	{
	public:
		std::string GetName() override { return "Native IBL"; }
		std::string GetShortName() override { return "NativeIBL"; }
		void PostPostLoad() override;
		json GetDiagnostics() override;
		void OnRuntimeDisabled() override { InvalidateGeneration(); Release(); }
		void ClearShaderCache() override { InvalidateGeneration(); Release(); }
		void Poll(uint32_t frame) { if (lighting) lighting->Poll(frame); }
		void InvalidateGeneration() { waitingForEnvironment = true; if (lighting) lighting->Reset(); }
		void Release() { lighting.reset(); }
		void Dispatch(const GraphicsBackend& backend,
			std::shared_ptr<const DX12Native::NativeScreenProbe::MaterialFrame> material,
			std::shared_ptr<const DX12Native::EnvironmentFrame> environment,
			const DX12Native::EnvironmentStamp& stamp,
			std::shared_ptr<const DX12Native::CharacterCasterFrame> characters, uint32_t quality,
			std::shared_ptr<const DX12Native::DirectLightingFrame> ssrDirect);
		std::shared_ptr<const DX12Native::IndirectLightingFrame> Get(uint32_t frame) const
		{
			return lighting ? lighting->Get(frame) : nullptr;
		}

	private:
		IBL::Settings settings;
		bool waitingForEnvironment = true;
		std::unique_ptr<DX12Native::NativeIndirectLighting> lighting;
	};
}

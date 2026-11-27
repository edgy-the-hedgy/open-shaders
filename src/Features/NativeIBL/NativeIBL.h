#pragma once

#include "Features/DX12/DX12NativeIndirectLighting.h"

namespace NativeIBL
{
	/** @brief Environment-only indirect lighting consumer; owns its DX12 resources. */
	class Producer
	{
	public:
		void Poll(uint32_t frame) { if (lighting) lighting->Poll(frame); }
		void Reset() { if (lighting) lighting->Reset(); }
		void Release() { lighting.reset(); }
		void Dispatch(const DX12Native::GraphicsBackend& backend,
			std::shared_ptr<const DX12Native::NativeScreenProbe::MaterialFrame> material,
			std::shared_ptr<const DX12Native::EnvironmentFrame> environment,
			const DX12Native::EnvironmentStamp& stamp,
			std::shared_ptr<const DX12Native::CharacterCasterFrame> characters);
		std::shared_ptr<const DX12Native::IndirectLightingFrame> Get(uint32_t frame) const
		{
			return lighting ? lighting->Get(frame) : nullptr;
		}

	private:
		std::unique_ptr<DX12Native::NativeIndirectLighting> lighting;
	};
}

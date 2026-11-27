#pragma once

#include "Features/DX12/DX12NativeEnvironment.h"

namespace NativeCubemaps
{
	/** @brief Owns native environment generations and fenced DX12 environment resources. */
	class Producer
	{
	public:
		DX12Native::EnvironmentStamp Prepare(uint32_t scope, uint32_t frame, bool& changed);
		void Capture(const GraphicsBackend& backend);
		void Reset();
		void Release() { environment.reset(); }
		std::shared_ptr<const DX12Native::EnvironmentFrame> Get() const { return environment ? environment->Get() : nullptr; }

	private:
		std::unique_ptr<DX12Native::NativeEnvironment> environment;
		DX12Native::EnvironmentStamp currentStamp{};
		uint64_t epoch = 1;
		float lastHours = 0.0f;
	};
}

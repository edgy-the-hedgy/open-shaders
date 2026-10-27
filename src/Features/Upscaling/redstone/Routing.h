#pragma once
#include "Features/ScreenSpaceGI.h"

namespace Redstone
{
	inline ScreenSpaceGI* activeSSGI = &globals::features::screenSpaceGI;
	inline void (ScreenSpaceGI::*drawSSGI)() = &ScreenSpaceGI::DrawSSGI;
	inline ID3D11ComputeShader* (*getCompositeShader)(bool, std::vector<std::pair<const char*, const char*>>&) = nullptr;
	inline bool privateCompositeSelected = false;
	inline void (*bindComposite)() = nullptr;
	inline void (*finishComposite)(bool) = nullptr;
}

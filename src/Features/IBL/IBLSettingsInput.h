#pragma once

#include "Features/IBL.h"

namespace IBLInput
{
	bool IsManagedByENB();
	bool IsDisabledForCurrentScene(const IBL::Settings& settings);
	// Does not gate native settings on the availability of raster IBL compute output.
	IBL::PerFrame ResolveSettings(const IBL::Settings& settings);
}

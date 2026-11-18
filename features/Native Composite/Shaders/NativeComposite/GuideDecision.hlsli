#ifndef NATIVE_COMPOSITE_GUIDE_DECISION_HLSLI
#define NATIVE_COMPOSITE_GUIDE_DECISION_HLSLI
#ifdef __cplusplus
#include <cstdint>
namespace NativeCompositeGuide
{
	using uint = uint32_t;
#endif
	inline bool SelectNativeGuide(bool a_nativeAvailable, bool a_rasterAvailable, bool a_withinTolerance, uint a_priority)
	{
		if (!a_nativeAvailable)
			return false;
		if (!a_rasterAvailable)
			return true;
		return a_priority == 0u ? !a_withinTolerance : a_withinTolerance;
	}
#ifdef __cplusplus
}
#endif
#endif

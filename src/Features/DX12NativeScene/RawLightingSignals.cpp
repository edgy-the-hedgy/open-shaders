#include "DirectionalTrace.h"

namespace DX12Native
{
    void DirectionalTrace::CaptureRawSignals(Frame& frame, uint32_t first, uint32_t count)
    {
        for (uint32_t i = first; i < first + count; ++i)
            if (frame.directSignals[i]) frame.rawSignals[i].copy_from(frame.directSignals[i]->resource.get());
        if (first == 0) ++rawDirectCaptures;
        else ++rawIndirectCaptures;
    }
}

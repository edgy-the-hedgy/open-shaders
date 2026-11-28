#pragma once
#include <cstdint>

namespace DX12Native
{
    // Update frequency is a request cadence, not a promise of same-frame
    // asynchronous deformation. Delayed poses are explicitly approximate.
    inline constexpr uint32_t kActorAsyncFrameAllowance = 8;
    inline constexpr uint32_t ActorCaptureInterval(uint32_t interval)
    {
        return interval == 2u ? 2u : 1u;
    }
    inline constexpr uint32_t ActorMaxPoseAge(uint32_t interval)
    {
        return kActorAsyncFrameAllowance + ActorCaptureInterval(interval) - 1u;
    }
    inline constexpr bool ActorPoseAgeAllowed(uint32_t now, uint32_t captured, uint32_t interval)
    {
        return now - captured <= ActorMaxPoseAge(interval);
    }
    inline constexpr bool ActorCaptureDue(uint32_t now, uint32_t previous, uint32_t interval, bool hasPrevious)
    {
        return !hasPrevious || now - previous >= ActorCaptureInterval(interval);
    }
    inline constexpr bool ActorRequestCompatible(uint32_t scope, uint32_t interval, uint32_t requestedScope, uint32_t requestedInterval)
    {
        return scope == requestedScope && ActorCaptureInterval(interval) == ActorCaptureInterval(requestedInterval);
    }
}

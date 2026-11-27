#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace NativeCubemaps::Policy
{
    inline constexpr uint32_t kSize = 256;
    inline constexpr uint32_t kMips = 9;
    inline constexpr uint32_t kCaptureInterval = 6;
    inline constexpr uint32_t kMaxHistoryGap = 120;
    inline constexpr float kMaxHistoryTranslation = 128.0f;

    constexpr bool CaptureDue(uint32_t previous, uint32_t current)
    {
        return previous == std::numeric_limits<uint32_t>::max() ||
            current - previous >= kCaptureInterval;
    }

    inline bool CanReuseHistory(bool sameEnvironment, uint32_t elapsed, float distanceSquared)
    {
        return sameEnvironment && elapsed && elapsed <= kMaxHistoryGap &&
            std::isfinite(distanceSquared) && distanceSquared >= 0 &&
            distanceSquared <= kMaxHistoryTranslation * kMaxHistoryTranslation;
    }
}

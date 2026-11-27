#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace NativeCubemaps::Policy
{
    inline constexpr uint32_t kSize = 256;
    inline constexpr uint32_t kMips = 9;
    inline constexpr uint32_t kCaptureInterval = 6;
    inline constexpr uint32_t kMipsPerStep = 3;
    enum class ProcessingPhase { History, Base, Reflection, Complete };
    struct ProcessingCursor
    {
        ProcessingPhase phase = ProcessingPhase::History;
        uint32_t mip = 0;
        constexpr uint32_t EndMip() const
        {
            const uint32_t end = mip + (mip ? kMipsPerStep : 1u);
            return end < kMips ? end : kMips;
        }
        constexpr void Advance(bool reflection)
        {
            if (phase == ProcessingPhase::History) { phase = ProcessingPhase::Base; mip = 0; return; }
            if (phase == ProcessingPhase::Complete) return;
            mip = EndMip();
            if (mip == kMips) {
                phase = phase == ProcessingPhase::Base && reflection ? ProcessingPhase::Reflection : ProcessingPhase::Complete;
                mip = 0;
            }
        }
    };
    inline constexpr uint32_t kMaxHistoryGap = 120;
    inline constexpr uint32_t kMaxPublicationAge = kMaxHistoryGap;
    inline constexpr float kMaxHistoryTranslation = 128.0f;

    constexpr bool CaptureDue(uint32_t previous, uint32_t current)
    {
        return previous == std::numeric_limits<uint32_t>::max() ||
            current - previous >= kCaptureInterval;
    }

    constexpr bool PublicationFresh(bool sameEnvironment, uint32_t observed, uint32_t requested)
    {
        return sameEnvironment && requested - observed <= kMaxPublicationAge;
    }

    inline bool CanReuseHistory(bool sameEnvironment, uint32_t elapsed, float distanceSquared)
    {
        return sameEnvironment && elapsed && elapsed <= kMaxHistoryGap &&
            std::isfinite(distanceSquared) && distanceSquared >= 0 &&
            distanceSquared <= kMaxHistoryTranslation * kMaxHistoryTranslation;
    }
}

#pragma once

#include <algorithm>
#include <array>
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
    struct CubeFaceHistory
    {
        static constexpr uint32_t kFaces=6,kAllFaces=(1u<<kFaces)-1u;
        std::array<uint32_t,kFaces> frames{};
        uint32_t valid=0;
        constexpr void Observe(uint32_t mask,uint32_t frame)
        {
            for(uint32_t i=0;i<kFaces;++i)if(mask&(1u<<i)){frames[i]=frame;valid|=1u<<i;}
        }
        constexpr bool Fresh(uint32_t frame) const
        {
            if(valid!=kAllFaces)return false;
            for(auto face:frames)if(frame-face>kMaxPublicationAge)return false;
            return true;
        }
    };

    inline bool ClockDiscontinuity(float previous, float current)
    {
        // A normal advancing game clock must not invalidate an in-flight cube.
        return std::isfinite(previous) && std::isfinite(current) &&
            (current < previous || current - previous > 0.25f);
    }

    inline bool LightingDiscontinuity(float previous,float current)
    {
        if(!std::isfinite(previous)||!std::isfinite(current))return true;
        if(previous<0||current<0)return true;
        if(previous<0.001f && current<0.001f)return false;
        return std::abs(std::log2(std::max(current,0.0001f)/std::max(previous,0.0001f)))>=1.0f;
    }

    // A stencil draw is positive evidence of water in the main view, including
    // custom water in an interior whose CELL flag is missing. Exterior water
    // can belong to an adjacent cell or LOD, not necessarily the player's cell.
    struct WaterDrawEvidence
    {
        uint32_t engineFrame=0, pluginFrame=0, cell=0;
        bool observed=false;
        constexpr void Observe(uint32_t engine, uint32_t plugin, uint32_t cellID)
        {
            *this={engine,plugin,cellID,cellID!=0};
        }
        constexpr bool Matches(uint32_t engine, uint32_t plugin, uint32_t cellID) const
        {
            return observed && cellID!=0 && cell==cellID &&
                engineFrame==engine && pluginFrame==plugin;
        }
    };
    constexpr bool WaterCellEligible(uint32_t cellID, bool interior, bool hasWaterFlag, bool observedWater)
    {
        return cellID!=0 && (!interior || hasWaterFlag || observedWater);
    }

    struct WaterHistoryIdentity
    {
        uint32_t engineFrame=0,pluginFrame=0,width=0,height=0,outWidth=0,outHeight=0,scope=0,quality=0,encoding=0;
        uintptr_t camera=0;
        uint64_t geometry=0,material=0,environmentEpoch=0,environmentRevision=0;
        float radius=0;
        bool reset=true;
        constexpr bool CanReuse(const WaterHistoryIdentity& old) const
        {
            return !reset && engineFrame-old.engineFrame==1u && pluginFrame-old.pluginFrame==1u &&
                camera && camera==old.camera && scope && scope==old.scope &&
                width && height && outWidth && outHeight && width==old.width && height==old.height &&
                outWidth==old.outWidth && outHeight==old.outHeight && quality==old.quality && encoding==old.encoding &&
                geometry==old.geometry && material==old.material && radius==old.radius &&
                environmentEpoch==old.environmentEpoch && environmentRevision==old.environmentRevision;
        }
    };

    constexpr bool CaptureDue(uint32_t previous, uint32_t current, uint32_t interval=kCaptureInterval)
    {
        return previous == std::numeric_limits<uint32_t>::max() ||
            current - previous >= (interval < 1u ? 1u : interval > 60u ? 60u : interval);
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

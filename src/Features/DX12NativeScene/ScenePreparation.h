#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

class NativeScenePreparation
{
public:
    struct Status
    {
        bool busy = false;
        bool attempted = false;
        bool succeeded = false;
        uint32_t quality = 2;
        uint64_t generation = 0;
        std::string failure;
    };

    void Advance(bool a_inGuestRoom, bool a_autoEnabled, uint32_t a_quality);
    void RequestRebuild();
    Status GetStatus() const;
    bool CanLoad() const;

private:
    struct State
    {
        mutable std::mutex mutex;
        Status status;
        bool wasInRoom = false;
        bool pendingRebuild = false;
    };
    std::shared_ptr<State> state = std::make_shared<State>();
};

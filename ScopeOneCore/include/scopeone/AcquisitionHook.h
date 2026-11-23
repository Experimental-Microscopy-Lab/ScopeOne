#pragma once

#include <optional>

namespace scopeone::core
{
    // Points of every experiment event at which a client is asked what to do, as the acquisition
    // hooks of pycro-manager: before the hardware is moved for the event, and after the cameras
    // took its frames
    enum class HookPoint
    {
        BeforeEvent,
        AfterEvent
    };

    // The answer to one hook request. Skip is only taken before an event; the event then takes
    // no frames. Stop ends the experiment as if it had finished. The changes are only taken
    // before an event, and the session records the changed event.
    struct HookReply
    {
        enum class Action
        {
            Continue,
            Skip,
            Stop
        };

        Action action{Action::Continue};
        std::optional<double> x;
        std::optional<double> y;
        std::optional<double> z;
        std::optional<double> exposureMs;
    };
}

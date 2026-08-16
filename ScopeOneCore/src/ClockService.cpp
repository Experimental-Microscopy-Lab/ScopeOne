#include "scopeone/ClockService.h"

#include <chrono>

namespace scopeone::core
{
    ClockStamp ClockService::now() const
    {
        const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        ClockStamp stamp;
        stamp.ticks = ticks;
        stamp.hostMonotonicNs = ticks;
        stamp.clockDomain = QStringLiteral("scopeone.host.monotonic");
        stamp.source = QStringLiteral("HostEstimated");
        return stamp;
    }
}

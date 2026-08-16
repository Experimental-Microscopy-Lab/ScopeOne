#include "internal/FrameRouter.h"

#include "scopeone/ClockService.h"

namespace scopeone::core::internal
{
    FrameRouter::FrameRouter(scopeone::core::ClockService* clockService, QObject* parent)
        : QObject(parent)
          , m_clockService(clockService)
    {
    }

    void FrameRouter::publish(const scopeone::core::ImageFrame& frame)
    {
        scopeone::core::ImageFrame routedFrame(frame);
        if (!routedFrame.clockStamp.isValid() && m_clockService)
        {
            routedFrame.clockStamp = m_clockService->now();
        }
        emit frameReady(routedFrame);
    }
}

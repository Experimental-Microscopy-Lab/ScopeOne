#include "internal/FrameRouter.h"

namespace scopeone::core::internal
{
    FrameRouter::FrameRouter(QObject* parent)
        : QObject(parent)
    {
    }

    void FrameRouter::publish(const scopeone::core::ImageFrame& frame)
    {
        emit frameReady(frame);
    }
}

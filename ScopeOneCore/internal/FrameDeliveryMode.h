#pragma once

namespace scopeone::core::internal
{
    // Which frames a running camera hands on
    enum class FrameDeliveryMode
    {
        // The newest frame, for the live view
        PreviewLatest,
        // The newest frame each time one arrives, for live processing
        LatestOnly,
        // Every frame in order, for recording
        AllFrames
    };
}

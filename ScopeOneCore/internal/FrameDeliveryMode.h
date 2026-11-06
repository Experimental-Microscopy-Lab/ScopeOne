#pragma once

namespace scopeone::core::internal
{
    // The live view shows at most one frame of a camera in this interval, about 60 a second
    inline constexpr int kPreviewFrameIntervalMs = 16;

    // Which frames a running camera hands on
    enum class FrameDeliveryMode
    {
        // The newest frame, at most about 60 times a second, for the live view
        PreviewLatest,
        // The newest frame each time one arrives, for live processing
        LatestOnly,
        // Every frame in order, for recording
        AllFrames
    };
}

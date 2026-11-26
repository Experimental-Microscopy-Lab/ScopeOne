#pragma once

#include <atomic>

namespace scopeone::core::internal
{
    // The live view shows at most one frame of a camera in this interval, one refresh of the
    // display, so no frame is copied or drawn that the screen cannot show. Each process keeps its
    // own, and DriverHost gets that of the application with the delivery mode.
    inline std::atomic_int previewFrameIntervalMsValue{16};

    inline int previewFrameIntervalMs()
    {
        return previewFrameIntervalMsValue.load(std::memory_order_relaxed);
    }

    inline void setPreviewFrameIntervalMs(int intervalMs)
    {
        previewFrameIntervalMsValue.store(intervalMs < 1 ? 1 : intervalMs, std::memory_order_relaxed);
    }

    // Which frames a running camera hands on
    enum class FrameDeliveryMode
    {
        // The newest frame, at most once per preview interval, for the live view
        PreviewLatest,
        // The newest frame each time one arrives, for live processing
        LatestOnly,
        // Every frame in order, for recording
        AllFrames
    };
}

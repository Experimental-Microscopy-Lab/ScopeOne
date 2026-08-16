#pragma once

#include <QString>
#include <QtGlobal>

namespace scopeone::core::internal
{
    class CameraRuntimeControl
    {
    public:
        virtual ~CameraRuntimeControl() = default;

        virtual void setFrameDeliveryPaused(bool paused) = 0;
        virtual bool setRecordingFrameDeliveryEnabled(bool enabled) = 0;
        virtual bool setHighRateFrameDeliveryEnabled(bool enabled) = 0;
        virtual bool isProcessingFrameTokenCurrent(const QString& cameraId,
                                                   quint64 token) = 0;
        virtual void finishProcessingFrame(const QString& cameraId, quint64 token) = 0;
    };
}

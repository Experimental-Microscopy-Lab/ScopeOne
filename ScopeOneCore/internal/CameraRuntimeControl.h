#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace scopeone::core::internal
{
    class CameraRuntimeControl
    {
    public:
        virtual ~CameraRuntimeControl() = default;

        virtual void setFrameDeliveryPaused(const QStringList& cameraIds, bool paused) = 0;
        virtual bool setRecordingFrameDeliveryEnabled(const QStringList& cameraIds,
                                                      bool enabled) = 0;
        virtual bool setHighRateFrameDeliveryEnabled(const QStringList& cameraIds,
                                                     bool enabled) = 0;
        virtual bool isProcessingFrameTokenCurrent(const QString& cameraId,
                                                   quint64 token) = 0;
        virtual void finishProcessingFrame(const QString& cameraId, quint64 token) = 0;
    };
}

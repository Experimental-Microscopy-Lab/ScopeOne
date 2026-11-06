#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <atomic>
#include <deque>

#include "internal/FrameDeliveryMode.h"
#include "scopeone/ImageFrame.h"

namespace scopeone::core::internal
{
    // Hands the frames of every camera, whichever provider runs it, to the live view, to live
    // processing and to recording. The live view gets the newest frame of each camera once per pass
    // of the event loop, processing gets one frame of a camera at a time, and recording gets every
    // frame in order, in bounded batches.
    class CameraFrameDelivery final : public QObject
    {
        Q_OBJECT

    public:
        explicit CameraFrameDelivery(QObject* parent = nullptr);

        // Each camera is recorded or processed on its own, as others may keep their live view
        void setRecordingEnabled(const QStringList& cameraIds, bool enabled);
        bool recordingEnabled(const QString& cameraId) const;
        void setHighRateEnabled(const QStringList& cameraIds, bool enabled);
        bool highRateEnabled(const QString& cameraId) const;
        // The mode the cameras run in for what is enabled for them
        FrameDeliveryMode mode(const QString& cameraId) const;
        // Forgets the cameras, as the ids of a new configuration may name other cameras
        void removeCameras(const QStringList& cameraIds);

        // Whether the newest frame of a camera is worth reading now. Without recording or live
        // processing every newest frame is, as the live view takes it.
        bool wantsLatestFrame(const QString& cameraId);
        // Frames of one camera in order. acquiredFrameCount counts every frame the camera took
        // since the last call, including those that were not read.
        void deliverFrames(const QList<ImageFrame>& frames, quint64 acquiredFrameCount);

        bool isProcessingFrameCurrent(const QString& cameraId, quint64 token);
        void finishProcessingFrame(const QString& cameraId, quint64 token);

        // Ends recording delivery of every camera after one failed while it recorded
        void fail(const QString& message);
        void discardPendingFrames();

    signals:
        void frameReady(const scopeone::core::ImageFrame& frame);
        // Emitted on the thread that delivered the frame
        void processingFrameReady(const scopeone::core::ImageFrame& frame, quint64 token);
        void framesAcquired(const QString& cameraId, quint64 frameCount);
        void recordingFramesReady(const QList<scopeone::core::ImageFrame>& frames);
        void deliveryFailed(const QString& errorMessage, quint64 droppedFrames);

    private:
        quint64 tryAcquireProcessingFrame(const QString& cameraId);
        bool previewDue(const QString& cameraId) const;
        void flushPendingFrames();

        mutable QMutex m_mutex;
        QHash<QString, ImageFrame> m_pendingLatestFrames;
        QHash<QString, quint64> m_pendingAcquiredFrameCounts;
        QHash<QString, QElapsedTimer> m_previewTimers;
        std::deque<ImageFrame> m_pendingRecordingFrames;
        qint64 m_pendingRecordingBytes{0};
        quint64 m_pendingDroppedRecordingFrames{0};
        QString m_pendingDeliveryError;
        QSet<QString> m_recordingCameras;
        bool m_frameFlushQueued{false};

        mutable QMutex m_processingMutex;
        QSet<QString> m_highRateCameras;
        QHash<QString, quint64> m_processingTokens;
        quint64 m_nextProcessingToken{0};
    };

    // What a provider that runs cameras in ScopeOne offers HardwareRuntime, so their frames reach
    // the one CameraFrameDelivery
    class CameraFrameSource
    {
    public:
        virtual ~CameraFrameSource() = default;

        virtual void setFrameDelivery(CameraFrameDelivery* delivery) = 0;
        virtual bool setFrameDeliveryMode(const QStringList& cameraIds, FrameDeliveryMode mode) = 0;
        virtual void setFrameDeliveryPaused(const QStringList& cameraIds, bool paused) = 0;
    };
}

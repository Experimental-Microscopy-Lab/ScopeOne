#pragma once

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

        // Each camera is recorded or processed on its own, as others may keep their live view.
        // Enabling hands over the last preTriggerCount frames of each camera's pre-trigger buffer in
        // preTriggerFrames, taken under the same lock, so no frame falls between the two.
        void setRecordingEnabled(const QStringList& cameraIds, bool enabled,
                                 QList<ImageFrame>* preTriggerFrames = nullptr, int preTriggerCount = 0);
        bool recordingEnabled(const QString& cameraId) const;
        void setHighRateEnabled(const QStringList& cameraIds, bool enabled);
        bool highRateEnabled(const QString& cameraId) const;
        // Keeps the last frames of each camera for record triggers that start with frames from before
        // the threshold: frameCounts frames, and no more than byteLimits bytes, of each camera.
        // Cameras without a count keep nothing, and such a camera runs in AllFrames mode only
        // while it is recorded.
        void setPreTriggerFrames(const QHash<QString, int>& frameCounts, const QHash<QString, qint64>& byteLimits);
        bool preTriggerEnabled(const QString& cameraId) const;
        // The mode the cameras run in for what is enabled for them
        FrameDeliveryMode mode(const QString& cameraId) const;
        // Forgets the cameras, as the ids of a new configuration may name other cameras
        void removeCameras(const QStringList& cameraIds);

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
        struct PreTriggerBuffer;
        void keepPreTriggerFrame(PreTriggerBuffer& buffer, const ImageFrame& frame,
                                 std::deque<ImageFrame>& discardedFrames);
        static void trimPreTriggerBuffer(PreTriggerBuffer& buffer, const QString& cameraId,
                                         std::deque<ImageFrame>& discardedFrames);
        quint64 tryAcquireProcessingFrame(const ImageFrame& frame);
        void flushPendingFrames();

        mutable QMutex m_mutex;
        QHash<QString, ImageFrame> m_pendingLatestFrames;
        QHash<QString, quint64> m_pendingAcquiredFrameCounts;
        std::deque<ImageFrame> m_pendingRecordingFrames;
        qint64 m_pendingRecordingBytes{0};
        quint64 m_pendingDroppedRecordingFrames{0};
        QString m_pendingDeliveryError;
        QSet<QString> m_recordingCameras;
        bool m_frameFlushQueued{false};
        struct PreTriggerBuffer
        {
            int frameCount{0};
            qint64 byteLimit{0};
            std::deque<ImageFrame> frames;
            qint64 bytes{0};
            bool warned{false};
        };
        QHash<QString, PreTriggerBuffer> m_preTriggerBuffers;
        // Frames pushed out of the pre-trigger buffers, released together on the next flush
        // instead of one task for each delivered batch
        std::deque<ImageFrame> m_evictedPreTriggerFrames;

        mutable QMutex m_processingMutex;
        QSet<QString> m_highRateCameras;
        QHash<QString, quint64> m_processingTokens;
        // The newest frame of each camera that arrived while its processing was busy, which the
        // processing takes as soon as it finishes
        QHash<QString, ImageFrame> m_pendingProcessingFrames;
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

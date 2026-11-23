#include "internal/CameraFrameDelivery.h"

#include <QDateTime>
#include <QDebug>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThreadPool>

#include <algorithm>
#include <iterator>
#include <utility>

namespace scopeone::core::internal
{
    namespace
    {
        constexpr qint64 kMaximumPendingRecordingBytes = 256ll * 1024 * 1024;
        constexpr qint64 kMaximumRecordingFlushBytes = 16ll * 1024 * 1024;
        constexpr qsizetype kMaximumRecordingFlushFrames = 16;

        // Releases discarded frame buffers away from the thread that delivers frames
        void releaseFramesAsync(std::deque<ImageFrame>&& frames)
        {
            if (frames.empty())
            {
                return;
            }
            QThreadPool::globalInstance()->start([frames = std::move(frames)]() mutable { frames.clear(); });
        }
    }

    CameraFrameDelivery::CameraFrameDelivery(QObject* parent)
        : QObject(parent)
    {
        qRegisterMetaType<ImageFrame>("scopeone::core::ImageFrame");
    }

    // The queue starts empty with the first recorded camera and is dropped with the last
    void CameraFrameDelivery::setRecordingEnabled(const QStringList& cameraIds, bool enabled,
                                                  QList<ImageFrame>* preTriggerFrames, int preTriggerCount)
    {
        std::deque<ImageFrame> discardedFrames;
        {
            QMutexLocker lock(&m_mutex);
            const bool wasRecording = !m_recordingCameras.isEmpty();
            for (const QString& cameraId : cameraIds)
            {
                if (enabled) m_recordingCameras.insert(cameraId);
                else m_recordingCameras.remove(cameraId);
                // The buffer starts again after the recording, from the frames that follow it
                auto buffer = m_preTriggerBuffers.find(cameraId);
                if (enabled && buffer != m_preTriggerBuffers.end())
                {
                    std::deque<ImageFrame>& frames = buffer->frames;
                    const std::size_t taken = std::min<std::size_t>(frames.size(), std::max(preTriggerCount, 0));
                    if (preTriggerFrames)
                    {
                        std::copy(frames.end() - static_cast<std::ptrdiff_t>(taken), frames.end(),
                                  std::back_inserter(*preTriggerFrames));
                    }
                    std::move(frames.begin(), frames.end(), std::back_inserter(discardedFrames));
                    frames.clear();
                    buffer->bytes = 0;
                }
            }
            if (wasRecording == m_recordingCameras.isEmpty())
            {
                discardedFrames.swap(m_pendingRecordingFrames);
                m_pendingRecordingBytes = 0;
                m_pendingDroppedRecordingFrames = 0;
                m_pendingDeliveryError.clear();
            }
        }
        releaseFramesAsync(std::move(discardedFrames));
    }

    void CameraFrameDelivery::setPreTriggerFrames(const QHash<QString, int>& frameCounts,
                                                  const QHash<QString, qint64>& byteLimits)
    {
        std::deque<ImageFrame> discardedFrames;
        {
            QMutexLocker lock(&m_mutex);
            for (auto it = m_preTriggerBuffers.begin(); it != m_preTriggerBuffers.end();)
            {
                if (frameCounts.value(it.key()) > 0)
                {
                    ++it;
                    continue;
                }
                std::move(it->frames.begin(), it->frames.end(), std::back_inserter(discardedFrames));
                it = m_preTriggerBuffers.erase(it);
            }
            for (auto it = frameCounts.cbegin(); it != frameCounts.cend(); ++it)
            {
                if (it.value() <= 0)
                {
                    continue;
                }
                PreTriggerBuffer& buffer = m_preTriggerBuffers[it.key()];
                buffer.frameCount = it.value();
                buffer.byteLimit = byteLimits.value(it.key());
                buffer.warned = false;
                while (!buffer.frames.empty()
                       && (static_cast<int>(buffer.frames.size()) > buffer.frameCount || buffer.bytes > buffer.byteLimit))
                {
                    buffer.bytes -= buffer.frames.front().payloadByteCount();
                    discardedFrames.push_back(std::move(buffer.frames.front()));
                    buffer.frames.pop_front();
                }
            }
        }
        releaseFramesAsync(std::move(discardedFrames));
    }

    bool CameraFrameDelivery::preTriggerEnabled(const QString& cameraId) const
    {
        QMutexLocker lock(&m_mutex);
        return m_preTriggerBuffers.contains(cameraId);
    }

    bool CameraFrameDelivery::recordingEnabled(const QString& cameraId) const
    {
        QMutexLocker lock(&m_mutex);
        return m_recordingCameras.contains(cameraId);
    }

    // Changing the mode of a camera invalidates its processing frame still in flight
    void CameraFrameDelivery::setHighRateEnabled(const QStringList& cameraIds, bool enabled)
    {
        QMutexLocker lock(&m_processingMutex);
        for (const QString& cameraId : cameraIds)
        {
            if (enabled) m_highRateCameras.insert(cameraId);
            else m_highRateCameras.remove(cameraId);
            m_processingTokens.remove(cameraId);
        }
    }

    bool CameraFrameDelivery::highRateEnabled(const QString& cameraId) const
    {
        QMutexLocker lock(&m_processingMutex);
        return m_highRateCameras.contains(cameraId);
    }

    FrameDeliveryMode CameraFrameDelivery::mode(const QString& cameraId) const
    {
        return recordingEnabled(cameraId) || preTriggerEnabled(cameraId) ? FrameDeliveryMode::AllFrames
                                          : highRateEnabled(cameraId) ? FrameDeliveryMode::LatestOnly
                                                                      : FrameDeliveryMode::PreviewLatest;
    }

    void CameraFrameDelivery::removeCameras(const QStringList& cameraIds)
    {
        setRecordingEnabled(cameraIds, false);
        setHighRateEnabled(cameraIds, false);
        QHash<QString, int> frameCounts;
        QHash<QString, qint64> byteLimits;
        {
            QMutexLocker lock(&m_mutex);
            for (auto it = m_preTriggerBuffers.cbegin(); it != m_preTriggerBuffers.cend(); ++it)
            {
                if (!cameraIds.contains(it.key()))
                {
                    frameCounts.insert(it.key(), it->frameCount);
                    byteLimits.insert(it.key(), it->byteLimit);
                }
            }
        }
        setPreTriggerFrames(frameCounts, byteLimits);
    }

    bool CameraFrameDelivery::previewDue(const QString& cameraId) const
    {
        const auto it = m_previewTimers.constFind(cameraId);
        return it == m_previewTimers.cend()
            || !it->isValid()
            || it->elapsed() >= kPreviewFrameIntervalMs;
    }

    bool CameraFrameDelivery::wantsLatestFrame(const QString& cameraId)
    {
        {
            QMutexLocker lock(&m_processingMutex);
            if (!m_highRateCameras.contains(cameraId) || !m_processingTokens.contains(cameraId))
            {
                return true;
            }
        }
        QMutexLocker lock(&m_mutex);
        return m_recordingCameras.contains(cameraId) || previewDue(cameraId);
    }

    quint64 CameraFrameDelivery::tryAcquireProcessingFrame(const QString& cameraId)
    {
        QMutexLocker lock(&m_processingMutex);
        if (!m_highRateCameras.contains(cameraId) || m_processingTokens.contains(cameraId))
        {
            return 0;
        }
        m_processingTokens.insert(cameraId, ++m_nextProcessingToken);
        return m_nextProcessingToken;
    }

    bool CameraFrameDelivery::isProcessingFrameCurrent(const QString& cameraId, quint64 token)
    {
        QMutexLocker lock(&m_processingMutex);
        return m_highRateCameras.contains(cameraId) && m_processingTokens.value(cameraId) == token;
    }

    void CameraFrameDelivery::finishProcessingFrame(const QString& cameraId, quint64 token)
    {
        QMutexLocker lock(&m_processingMutex);
        const auto it = m_processingTokens.constFind(cameraId);
        if (it != m_processingTokens.cend() && it.value() == token)
        {
            m_processingTokens.remove(cameraId);
        }
    }

    void CameraFrameDelivery::deliverFrames(const QList<ImageFrame>& frames, quint64 acquiredFrameCount)
    {
        if (frames.isEmpty() || !frames.constLast().isValid())
        {
            return;
        }
        const ImageFrame& latestFrame = frames.constLast();
        const QString cameraId = latestFrame.cameraId;
        const bool highRate = highRateEnabled(cameraId);
        if (const quint64 token = tryAcquireProcessingFrame(cameraId))
        {
            emit processingFrameReady(latestFrame, token);
        }

        bool queueFlush = false;
        std::deque<ImageFrame> discardedFrames;
        {
            QMutexLocker lock(&m_mutex);
            // A camera that is not recorded skips frames for its live view, which are not lost
            bool recording = m_recordingCameras.contains(cameraId);
            const bool recordingWasEnabled = recording;
            quint64 validFrameCount = 0;
            quint64 queuedFrameCount = 0;
            const auto buffer = recording ? m_preTriggerBuffers.end() : m_preTriggerBuffers.find(cameraId);
            for (const ImageFrame& frame : frames)
            {
                if (!frame.isValid())
                {
                    continue;
                }
                ++validFrameCount;
                if (buffer != m_preTriggerBuffers.end())
                {
                    keepPreTriggerFrame(*buffer, frame, m_evictedPreTriggerFrames);
                }
                if (!recording)
                {
                    continue;
                }
                const qint64 frameBytes = frame.payloadByteCount();
                if (frameBytes > kMaximumPendingRecordingBytes - m_pendingRecordingBytes)
                {
                    if (m_pendingDeliveryError.isEmpty())
                    {
                        m_pendingDeliveryError =
                            QStringLiteral("Recording frame delivery exceeded the 256 MiB pending limit");
                    }
                    recording = false;
                    m_recordingCameras.clear();
                    continue;
                }
                m_pendingRecordingFrames.push_back(frame);
                m_pendingRecordingBytes += frameBytes;
                ++queuedFrameCount;
            }

            // Live processing reads frames faster than the view shows them, and a camera kept in
            // AllFrames mode for its pre-trigger buffer delivers them faster too
            const bool throttled = highRate || buffer != m_preTriggerBuffers.end();
            if (recordingWasEnabled || !throttled || previewDue(cameraId))
            {
                m_pendingLatestFrames.insert(cameraId, latestFrame);
                m_previewTimers[cameraId].start();
            }
            if (acquiredFrameCount > 0)
            {
                m_pendingAcquiredFrameCounts[cameraId] += acquiredFrameCount;
            }
            // Released here once enough have gathered, as a busy GUI thread would otherwise let
            // them pile up until its next flush
            if (static_cast<qsizetype>(m_evictedPreTriggerFrames.size()) >= kMaximumRecordingFlushFrames)
            {
                std::move(m_evictedPreTriggerFrames.begin(), m_evictedPreTriggerFrames.end(),
                          std::back_inserter(discardedFrames));
                m_evictedPreTriggerFrames.clear();
            }
            if (recording && acquiredFrameCount > validFrameCount && m_pendingDeliveryError.isEmpty())
            {
                m_pendingDeliveryError = QStringLiteral("Recording frame delivery detected dropped camera frames");
                m_recordingCameras.clear();
            }
            if (recordingWasEnabled && !m_pendingDeliveryError.isEmpty())
            {
                const quint64 submittedFrameCount = (std::max)(acquiredFrameCount, validFrameCount);
                m_pendingDroppedRecordingFrames += submittedFrameCount - queuedFrameCount;
                m_pendingDroppedRecordingFrames += static_cast<quint64>(m_pendingRecordingFrames.size());
                discardedFrames.swap(m_pendingRecordingFrames);
                m_pendingRecordingBytes = 0;
            }
            if (!m_frameFlushQueued
                && (!m_pendingLatestFrames.isEmpty()
                    || !m_pendingAcquiredFrameCounts.isEmpty()
                    || !m_pendingRecordingFrames.empty()
                    || !m_evictedPreTriggerFrames.empty()
                    || !m_pendingDeliveryError.isEmpty()))
            {
                m_frameFlushQueued = true;
                queueFlush = true;
            }
        }

        releaseFramesAsync(std::move(discardedFrames));
        if (queueFlush)
        {
            QMetaObject::invokeMethod(this, [this]() { flushPendingFrames(); }, Qt::QueuedConnection);
        }
    }

    // Keeps one frame in a pre-trigger buffer, dropping the oldest beyond its frame count or bytes.
    // A frame that does not follow the last one, or of another size or format, starts the buffer
    // again, so that it holds only frames a recording can take as one run: the recording fails on a
    // skipped frame, and frames from before a change of the ROI or the binning do not belong to it.
    void CameraFrameDelivery::keepPreTriggerFrame(PreTriggerBuffer& buffer, const ImageFrame& frame,
                                                  std::deque<ImageFrame>& discardedFrames)
    {
        if (!buffer.frames.empty())
        {
            const ImageFrame& last = buffer.frames.back();
            if (last.width != frame.width || last.height != frame.height || last.pixelFormat != frame.pixelFormat
                || frame.frameIndex != last.frameIndex + 1)
            {
                std::move(buffer.frames.begin(), buffer.frames.end(), std::back_inserter(discardedFrames));
                buffer.frames.clear();
                buffer.bytes = 0;
            }
        }
        buffer.frames.push_back(frame);
        // Stamped as it is taken, as the recording would otherwise stamp every buffered frame
        // with the time it started
        if (buffer.frames.back().timestampNs == 0)
        {
            buffer.frames.back().timestampNs = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;
        }
        buffer.bytes += frame.payloadByteCount();
        while (static_cast<int>(buffer.frames.size()) > buffer.frameCount
               || (buffer.bytes > buffer.byteLimit && buffer.frames.size() > 1))
        {
            if (static_cast<int>(buffer.frames.size()) <= buffer.frameCount && !buffer.warned)
            {
                // The frames grew past the size they had when the triggers were set
                buffer.warned = true;
                qWarning().noquote() << QStringLiteral("The pre-trigger buffer of %1 holds fewer than %2 frames at "
                                                       "the current frame size")
                                            .arg(frame.cameraId).arg(buffer.frameCount);
            }
            buffer.bytes -= buffer.frames.front().payloadByteCount();
            discardedFrames.push_back(std::move(buffer.frames.front()));
            buffer.frames.pop_front();
        }
    }

    void CameraFrameDelivery::fail(const QString& message)
    {
        std::deque<ImageFrame> discardedFrames;
        bool queueFlush = false;
        {
            QMutexLocker lock(&m_mutex);
            if (m_recordingCameras.isEmpty())
            {
                return;
            }
            m_recordingCameras.clear();
            if (m_pendingDeliveryError.isEmpty())
            {
                m_pendingDeliveryError = message;
            }
            m_pendingDroppedRecordingFrames += static_cast<quint64>(m_pendingRecordingFrames.size());
            discardedFrames.swap(m_pendingRecordingFrames);
            m_pendingRecordingBytes = 0;
            queueFlush = !std::exchange(m_frameFlushQueued, true);
        }
        releaseFramesAsync(std::move(discardedFrames));
        if (queueFlush)
        {
            QMetaObject::invokeMethod(this, [this]() { flushPendingFrames(); }, Qt::QueuedConnection);
        }
    }

    void CameraFrameDelivery::discardPendingFrames()
    {
        QMutexLocker lock(&m_mutex);
        m_pendingLatestFrames.clear();
        m_pendingAcquiredFrameCounts.clear();
    }

    // Delivers one bounded batch without holding the thread for long
    void CameraFrameDelivery::flushPendingFrames()
    {
        QHash<QString, ImageFrame> latestFrames;
        QHash<QString, quint64> acquiredFrameCounts;
        QList<ImageFrame> recordingFrames;
        QString deliveryError;
        quint64 droppedRecordingFrames = 0;
        bool queueNextFlush = false;
        std::deque<ImageFrame> evictedFrames;
        {
            QMutexLocker lock(&m_mutex);
            evictedFrames.swap(m_evictedPreTriggerFrames);
            latestFrames.swap(m_pendingLatestFrames);
            acquiredFrameCounts.swap(m_pendingAcquiredFrameCounts);
            deliveryError.swap(m_pendingDeliveryError);
            droppedRecordingFrames = std::exchange(m_pendingDroppedRecordingFrames, 0);
            qint64 flushBytes = 0;
            while (!m_pendingRecordingFrames.empty() && recordingFrames.size() < kMaximumRecordingFlushFrames)
            {
                const qint64 frameBytes = m_pendingRecordingFrames.front().payloadByteCount();
                if (!recordingFrames.isEmpty() && flushBytes + frameBytes > kMaximumRecordingFlushBytes)
                {
                    break;
                }
                flushBytes += frameBytes;
                recordingFrames.append(std::move(m_pendingRecordingFrames.front()));
                m_pendingRecordingFrames.pop_front();
            }
            m_pendingRecordingBytes -= flushBytes;
            queueNextFlush = !m_pendingRecordingFrames.empty();
            m_frameFlushQueued = queueNextFlush;
        }
        releaseFramesAsync(std::move(evictedFrames));

        for (auto it = acquiredFrameCounts.cbegin(); it != acquiredFrameCounts.cend(); ++it)
        {
            emit framesAcquired(it.key(), it.value());
        }
        for (auto it = latestFrames.cbegin(); it != latestFrames.cend(); ++it)
        {
            emit frameReady(it.value());
        }
        if (!deliveryError.isEmpty())
        {
            emit deliveryFailed(deliveryError, droppedRecordingFrames);
        }
        else if (!recordingFrames.isEmpty())
        {
            emit recordingFramesReady(recordingFrames);
        }
        if (queueNextFlush)
        {
            QMetaObject::invokeMethod(this, [this]() { flushPendingFrames(); }, Qt::QueuedConnection);
        }
    }
}

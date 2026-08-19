#include "internal/CameraBackend.h"

#include <QDebug>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThreadPool>
#include <utility>

namespace scopeone::core::internal
{
    namespace
    {
        constexpr qint64 kMaximumPendingRecordingBytes = 256ll * 1024 * 1024;
        constexpr qint64 kMaximumRecordingFlushBytes = 16ll * 1024 * 1024;
        constexpr qsizetype kMaximumRecordingFlushFrames = 16;

        // Releases discarded frame buffers away from the backend thread
        void releaseFramesAsync(std::deque<scopeone::core::ImageFrame>&& frames)
        {
            if (frames.empty())
            {
                return;
            }
            QThreadPool::globalInstance()->start(
                [frames = std::move(frames)]() mutable { frames.clear(); });
        }
    }

    // Enables processing admission and invalidates outstanding permits
    void ProcessingFrameGate::setEnabled(bool enabled)
    {
        QMutexLocker lock(&m_mutex);
        if (m_enabled == enabled)
        {
            return;
        }
        m_enabled = enabled;
        m_inFlightTokens.clear();
    }

    // Grants one processing permit per camera
    quint64 ProcessingFrameGate::tryAcquire(const QString& cameraId)
    {
        QMutexLocker lock(&m_mutex);
        if (!m_enabled || cameraId.isEmpty() || m_inFlightTokens.contains(cameraId))
        {
            return 0;
        }
        ++m_nextToken;
        m_inFlightTokens.insert(cameraId, m_nextToken);
        return m_nextToken;
    }

    // Checks whether a permit still belongs to the active request
    bool ProcessingFrameGate::isCurrent(const QString& cameraId, quint64 token)
    {
        QMutexLocker lock(&m_mutex);
        return m_enabled && m_inFlightTokens.value(cameraId) == token;
    }

    // Releases a permit only when its generation still matches
    void ProcessingFrameGate::release(const QString& cameraId, quint64 token)
    {
        QMutexLocker lock(&m_mutex);
        const auto it = m_inFlightTokens.constFind(cameraId);
        if (it != m_inFlightTokens.constEnd() && it.value() == token)
        {
            m_inFlightTokens.remove(cameraId);
        }
    }

    CameraBackend::CameraBackend(ProcessingFrameGate& processingFrameGate,
                                 QObject* parent)
        : QObject(parent)
          , m_processingFrameGate(processingFrameGate)
    {
    }

    CameraBackend::~CameraBackend() = default;

    bool CameraBackend::configureNativeCamera(const std::shared_ptr<CMMCore>&,
                                              const QString&,
                                              double)
    {
        return false;
    }

    bool CameraBackend::addDriverHostCamera(const QString&,
                                       const QString&,
                                       const QString&,
                                       const QStringList&,
                                       const QStringList&,
                                       double)
    {
        return false;
    }

    bool CameraBackend::captureEventFrame(const QString&, ImageFrame&, int)
    {
        return false;
    }

    // Starts or invalidates one generation of lossless recording delivery
    bool CameraBackend::setRecordingFrameDeliveryEnabled(bool enabled)
    {
        std::deque<ImageFrame> discardedFrames;
        {
            QMutexLocker lock(&m_frameDeliveryMutex);
            if (enabled && m_recordingFrameDeliveryToken.load(std::memory_order_relaxed) != 0)
            {
                return true;
            }

            const quint64 token = enabled ? ++m_nextRecordingFrameDeliveryToken : 0;
            m_recordingFrameDeliveryToken.store(token, std::memory_order_relaxed);
            discardedFrames.swap(m_pendingRecordingFrames);
            m_pendingRecordingBytes = 0;
            m_pendingDroppedRecordingFrames = 0;
            m_pendingDeliveryError.clear();
        }
        releaseFramesAsync(std::move(discardedFrames));
        return true;
    }

    // Selects full speed latest frame delivery for live processing consumers
    bool CameraBackend::setHighRateFrameDeliveryEnabled(bool enabled)
    {
        m_processingFrameGate.setEnabled(enabled);
        m_highRateFrameDeliveryEnabled.store(enabled, std::memory_order_relaxed);
        return true;
    }

    bool CameraBackend::getExposure(const QString& cameraIdOrAll, double& exposureMs) const
    {
        QString cameraId;
        if (!resolvePrimaryCameraId(cameraIdOrAll, cameraId))
        {
            return false;
        }
        return readExposureFor(cameraId, exposureMs);
    }

    bool CameraBackend::setExposure(const QString& cameraIdOrAll, double exposureMs)
    {
        const QStringList cameraIds = resolveTargetCameraIds(cameraIdOrAll);
        if (cameraIds.isEmpty())
        {
            return false;
        }

        for (const QString& cameraId : cameraIds)
        {
            if (!applyWithPreviewRestart(cameraId,
                                         [this, cameraId, exposureMs]()
                                         {
                                             return writeExposureFor(cameraId, exposureMs);
                                         }))
            {
                return false;
            }
        }
        return true;
    }

    QStringList CameraBackend::listProperties(const QString& cameraId)
    {
        QString resolvedCameraId;
        if (!resolvePrimaryCameraId(cameraId, resolvedCameraId))
        {
            return {};
        }
        return listPropertiesFor(resolvedCameraId);
    }

    bool CameraBackend::readPropertyDetails(const QString& cameraId,
                                            const QString& name,
                                            bool fromCache,
                                            CameraPropertyReadback& readback)
    {
        QString resolvedCameraId;
        if (!resolvePrimaryCameraId(cameraId, resolvedCameraId))
        {
            return false;
        }
        return readPropertyDetailsFor(resolvedCameraId, name, fromCache, readback);
    }

    bool CameraBackend::setProperty(const QString& cameraId,
                                    const QString& name,
                                    const QString& value,
                                    QString* errorMessage)
    {
        QString resolvedCameraId;
        if (!resolvePrimaryCameraId(cameraId, resolvedCameraId))
        {
            return false;
        }
        return applyWithPreviewRestart(
            resolvedCameraId,
            [this, resolvedCameraId, name, value, errorMessage]()
            {
                return setPropertyFor(resolvedCameraId, name, value, errorMessage);
            });
    }

    bool CameraBackend::setROI(const QString& cameraId, int x, int y, int width, int height)
    {
        QString resolvedCameraId;
        if (!resolvePrimaryCameraId(cameraId, resolvedCameraId))
        {
            return false;
        }
        return applyWithPreviewRestart(
            resolvedCameraId,
            [this, resolvedCameraId, x, y, width, height]()
            {
                return setROIFor(resolvedCameraId, x, y, width, height);
            });
    }

    bool CameraBackend::clearROI(const QString& cameraId)
    {
        QString resolvedCameraId;
        if (!resolvePrimaryCameraId(cameraId, resolvedCameraId))
        {
            return false;
        }
        return applyWithPreviewRestart(
            resolvedCameraId,
            [this, resolvedCameraId]() { return clearROIFor(resolvedCameraId); });
    }

    bool CameraBackend::getROI(const QString& cameraId, int& x, int& y, int& width, int& height)
    {
        QString resolvedCameraId;
        if (!resolvePrimaryCameraId(cameraId, resolvedCameraId))
        {
            return false;
        }
        return getROIFor(resolvedCameraId, x, y, width, height);
    }

    bool CameraBackend::applyWithPreviewRestart(const QString& cameraId,
                                                const std::function<bool()>& operation)
    {
        const bool wasRunning = isPreviewRunning(cameraId);
        if (wasRunning && !stopPreviewFor(cameraId))
        {
            return false;
        }

        const bool ok = operation();
        if (wasRunning && !startPreviewFor(cameraId))
        {
            return false;
        }
        return ok;
    }

    void CameraBackend::notifyPreviewStarted(const QString& cameraId)
    {
        emit previewStateChanged(true);
        qInfo().noquote() << QString("Preview started for camera '%1'").arg(cameraId);
    }

    void CameraBackend::notifyPreviewStopped()
    {
        if (!hasRunningCamera())
        {
            emit previewStateChanged(false);
            qInfo().noquote() << "All cameras stopped";
        }
    }

    // Routes one producer batch to processing, preview, and recording consumers
    void CameraBackend::submitFrames(const QList<ImageFrame>& frames,
                                     quint64 acquiredFrameCount,
                                     quint64 recordingToken)
    {
        bool queueFlush = false;
        std::deque<ImageFrame> discardedFrames;
        {
            QMutexLocker lock(&m_frameDeliveryMutex);
            bool recordingDeliveryEnabled =
                recordingToken != 0
                && recordingToken == m_recordingFrameDeliveryToken.load(std::memory_order_relaxed);
            const bool recordingWasEnabled = recordingDeliveryEnabled;
            QString acquiredCameraId;
            quint64 validFrameCount = 0;
            quint64 queuedFrameCount = 0;
            for (const ImageFrame& frame : frames)
            {
                const QString cameraId = frame.cameraId.trimmed();
                if (!frame.isValid() || cameraId.isEmpty())
                {
                    continue;
                }

                ImageFrame normalizedFrame(frame);
                normalizedFrame.cameraId = cameraId;
                acquiredCameraId = cameraId;
                ++validFrameCount;
                m_pendingLatestFrames.insert(cameraId, normalizedFrame);
                if (!recordingDeliveryEnabled)
                {
                    continue;
                }

                const qint64 frameBytes = normalizedFrame.payloadByteCount();
                if (frameBytes > kMaximumPendingRecordingBytes - m_pendingRecordingBytes)
                {
                    if (m_pendingDeliveryError.isEmpty())
                    {
                        m_pendingDeliveryError = QStringLiteral(
                            "Recording frame delivery exceeded the 256 MiB pending limit");
                    }
                    recordingDeliveryEnabled = false;
                    m_recordingFrameDeliveryToken.store(0, std::memory_order_relaxed);
                    continue;
                }

                m_pendingRecordingFrames.push_back(std::move(normalizedFrame));
                m_pendingRecordingBytes += frameBytes;
                ++queuedFrameCount;
            }

            if (!acquiredCameraId.isEmpty() && acquiredFrameCount > 0)
            {
                m_pendingAcquiredFrameCounts[acquiredCameraId] += acquiredFrameCount;
            }
            if (recordingDeliveryEnabled
                && acquiredFrameCount > validFrameCount
                && m_pendingDeliveryError.isEmpty())
            {
                m_pendingDeliveryError = QStringLiteral(
                    "Recording frame delivery detected dropped camera frames");
                m_recordingFrameDeliveryToken.store(0, std::memory_order_relaxed);
            }
            if (recordingWasEnabled && !m_pendingDeliveryError.isEmpty())
            {
                const quint64 submittedFrameCount = acquiredFrameCount > 0
                                                        ? acquiredFrameCount
                                                        : validFrameCount;
                if (submittedFrameCount > queuedFrameCount)
                {
                    m_pendingDroppedRecordingFrames += submittedFrameCount - queuedFrameCount;
                }
                m_pendingDroppedRecordingFrames +=
                    static_cast<quint64>(m_pendingRecordingFrames.size());
                discardedFrames.swap(m_pendingRecordingFrames);
                m_pendingRecordingBytes = 0;
            }

            if ((!m_pendingLatestFrames.isEmpty()
                 || !m_pendingAcquiredFrameCounts.isEmpty()
                 || !m_pendingRecordingFrames.empty()
                 || !m_pendingDeliveryError.isEmpty())
                && !m_frameFlushQueued)
            {
                m_frameFlushQueued = true;
                queueFlush = true;
            }
        }

        releaseFramesAsync(std::move(discardedFrames));

        if (queueFlush)
        {
            QMetaObject::invokeMethod(
                this,
                [this]() { flushPendingFrames(); },
                Qt::QueuedConnection);
        }
    }

    // Publishes one admitted frame to the processing pipeline
    void CameraBackend::submitProcessingFrame(const ImageFrame& frame, quint64 token)
    {
        emit processingFrameReady(frame, token);
    }

    // Requests one processing permit for a camera
    quint64 CameraBackend::tryAcquireProcessingFrame(const QString& cameraId)
    {
        return m_processingFrameGate.tryAcquire(cameraId);
    }

    // Returns one processing permit to the shared gate
    void CameraBackend::releaseProcessingFrame(const QString& cameraId, quint64 token)
    {
        m_processingFrameGate.release(cameraId, token);
    }

    // Drops coalesced preview frames after preview state changes
    void CameraBackend::discardPendingPreviewFrames()
    {
        QMutexLocker lock(&m_frameDeliveryMutex);
        m_pendingLatestFrames.clear();
        m_pendingAcquiredFrameCounts.clear();
    }

    // Reports whether a recording delivery generation is active
    bool CameraBackend::recordingFrameDeliveryEnabled() const
    {
        return recordingFrameDeliveryToken() != 0;
    }

    // Returns the generation accepted by the recording consumer
    quint64 CameraBackend::recordingFrameDeliveryToken() const
    {
        return m_recordingFrameDeliveryToken.load(std::memory_order_relaxed);
    }

    // Reports whether processing consumes frames independently of display rate
    bool CameraBackend::highRateFrameDeliveryEnabled() const
    {
        return m_highRateFrameDeliveryEnabled.load(std::memory_order_relaxed);
    }

    // Delivers one bounded batch without monopolizing the backend thread
    void CameraBackend::flushPendingFrames()
    {
        QHash<QString, ImageFrame> latestFrames;
        QHash<QString, quint64> acquiredFrameCounts;
        QList<ImageFrame> recordingFrames;
        QString deliveryError;
        quint64 droppedRecordingFrames = 0;
        bool queueNextFlush = false;
        {
            QMutexLocker lock(&m_frameDeliveryMutex);
            latestFrames.swap(m_pendingLatestFrames);
            acquiredFrameCounts.swap(m_pendingAcquiredFrameCounts);
            deliveryError.swap(m_pendingDeliveryError);
            droppedRecordingFrames = m_pendingDroppedRecordingFrames;
            m_pendingDroppedRecordingFrames = 0;
            qint64 flushBytes = 0;
            while (!m_pendingRecordingFrames.empty()
                   && recordingFrames.size() < kMaximumRecordingFlushFrames)
            {
                const qint64 frameBytes = m_pendingRecordingFrames.front().payloadByteCount();
                if (!recordingFrames.isEmpty()
                    && flushBytes + frameBytes > kMaximumRecordingFlushBytes)
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

        for (auto it = acquiredFrameCounts.constBegin(); it != acquiredFrameCounts.constEnd(); ++it)
        {
            emit rawFramesAcquired(it.key(), it.value());
        }
        for (auto it = latestFrames.constBegin(); it != latestFrames.constEnd(); ++it)
        {
            emit rawFrameReady(it.value());
        }
        if (!deliveryError.isEmpty())
        {
            recordingFrames.clear();
            emit frameDeliveryFailed(deliveryError, droppedRecordingFrames);
        }
        else if (!recordingFrames.isEmpty())
        {
            emit recordingFramesReady(recordingFrames);
        }
        if (queueNextFlush)
        {
            QMetaObject::invokeMethod(
                this,
                [this]() { flushPendingFrames(); },
                Qt::QueuedConnection);
        }
    }
}

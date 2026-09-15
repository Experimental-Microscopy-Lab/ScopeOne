#include "internal/MDAManager.h"

#include "scopeone/CameraProvider.h"
#include "scopeone/HardwareCapabilities.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <future>
#include <utility>
#include <vector>

namespace scopeone::core::internal
{
    namespace
    {
        quint64 currentTimestampNs()
        {
            return static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;
        }
    }

    // Creates the MDA manager and registers queued signal types
    MDAManager::MDAManager(QObject* parent)
        : QObject(parent)
    {
        qRegisterMetaType<scopeone::core::internal::MDAOutput>("scopeone::core::internal::MDAOutput");
        m_threadPool.setMaxThreadCount(1);
    }

    // Cancels and joins the acquisition worker before teardown
    MDAManager::~MDAManager()
    {
        cancelAndWait();
    }

    // Connects MDA capture to the active camera backend
    void MDAManager::setCameraProvider(CameraProvider* cameraProvider)
    {
        m_cameraProvider = cameraProvider;
    }

    void MDAManager::setStageProvider(StageProvider* stageProvider)
    {
        m_stageProvider = stageProvider;
    }

    // Starts one immutable acquisition event sequence
    bool MDAManager::start(const QList<AcquisitionEvent>& events, bool block)
    {
        if (m_running.exchange(true))
        {
            emit sequenceError(QStringLiteral("MDA already running"));
            return false;
        }
        if (events.isEmpty())
        {
            m_running.store(false);
            emit sequenceError(QStringLiteral("MDA event list is empty"));
            return false;
        }

        m_cancelRequested.store(false);
        if (block)
        {
            runSequence(events);
            return true;
        }

        m_threadPool.start([this, events]()
        {
            runSequence(events);
        });
        return true;
    }

    // Requests cancellation for the active MDA run
    void MDAManager::requestCancel()
    {
        m_cancelRequested.store(true);
    }

    // Cancels and joins the active acquisition worker
    void MDAManager::cancelAndWait()
    {
        requestCancel();
        m_threadPool.waitForDone();
    }

    // Executes a precomputed event sequence in order
    void MDAManager::runSequence(QList<AcquisitionEvent> events)
    {
        if (!m_cameraProvider)
        {
            m_running.store(false);
            emit sequenceError(QStringLiteral("Camera provider not available"));
            return;
        }

        QElapsedTimer timer;
        timer.start();
        const qint64 sequenceOriginMs = events.first().minimumStartTimeMs;

        for (const AcquisitionEvent& event : events)
        {
            const qint64 targetMs = event.minimumStartTimeMs - sequenceOriginMs;
            while (timer.elapsed() < targetMs)
            {
                if (m_cancelRequested.load())
                {
                    m_running.store(false);
                    emit sequenceCanceled();
                    return;
                }
                const qint64 remaining = targetMs - timer.elapsed();
                QThread::msleep(static_cast<unsigned long>((std::min<qint64>)(5, remaining)));
            }

            if (m_cancelRequested.load())
            {
                m_running.store(false);
                emit sequenceCanceled();
                return;
            }

            MDAOutput output;
            output.event = event;
            output.startedTimestampNs = currentTimestampNs();
            QString errorMessage;
            output.succeeded = setupEvent(event, &errorMessage)
                && execEvent(event, output, &errorMessage);
            output.completedTimestampNs = currentTimestampNs();
            output.errorMessage = output.succeeded
                                      ? QString()
                                      : (errorMessage.isEmpty()
                                             ? QStringLiteral("Failed to execute acquisition event")
                                             : errorMessage);
            emit eventFinished(output);

            if (!output.succeeded)
            {
                m_running.store(false);
                emit sequenceError(output.errorMessage);
                return;
            }
        }

        m_running.store(false);
        emit sequenceFinished();
    }

    // Moves hardware into place before capture
    bool MDAManager::setupEvent(const AcquisitionEvent& event, QString* errorMessage)
    {
        if (event.exposureMs > 0.0
            && !setExposure(event.cameraIds, event.exposureMs, errorMessage))
        {
            return false;
        }
        if (event.hasXY && !moveXY(event.x, event.y, errorMessage))
        {
            return false;
        }
        if (event.hasZ && !moveZ(event.z, errorMessage))
        {
            return false;
        }

        return true;
    }

    // Routes one event to the active capture implementation
    bool MDAManager::execEvent(const AcquisitionEvent& event, MDAOutput& output, QString* errorMessage)
    {
        if (!m_cameraProvider)
        {
            if (errorMessage) *errorMessage = QStringLiteral("Camera provider not available");
            return false;
        }
        if (event.cameraIds.isEmpty())
        {
            if (errorMessage) *errorMessage = QStringLiteral("No camera selected for acquisition event");
            return false;
        }
        return captureCameras(event, output, errorMessage);
    }

    // Captures one event across all selected camera providers
    bool MDAManager::captureCameras(const AcquisitionEvent& event,
                                    MDAOutput& output,
                                    QString* errorMessage)
    {
        const int captureTimeoutMs = static_cast<int>((std::max)(1500.0, event.exposureMs * 4.0 + 500.0));

        struct CaptureResult
        {
            QString cameraId;
            ImageFrame frame;
            bool ok{false};
            QString error;
        };

        std::vector<std::future<CaptureResult>> futures;
        futures.reserve(static_cast<size_t>(event.cameraIds.size()));
        for (const QString& cameraId : event.cameraIds)
        {
            futures.emplace_back(std::async(std::launch::async, [this, cameraId, captureTimeoutMs]()
            {
                CaptureResult result;
                result.cameraId = cameraId;
                if (!m_cameraProvider->captureEventFrame(cameraId, result.frame, captureTimeoutMs))
                {
                    result.error = QStringLiteral("Failed to capture frame from camera: %1").arg(cameraId);
                    return result;
                }
                if (!result.frame.isValid())
                {
                    result.error = QStringLiteral("Empty frame from camera: %1").arg(cameraId);
                    return result;
                }
                result.ok = true;
                return result;
            }));
        }

        for (auto& future : futures)
        {
            CaptureResult result = future.get();
            if (!result.ok)
            {
                if (errorMessage) *errorMessage = result.error;
                return false;
            }
            result.frame.cameraId = result.cameraId;
            output.frames.insert(result.cameraId, std::move(result.frame));
        }
        return true;
    }

    // Applies exposure for the next event
    bool MDAManager::setExposure(const QStringList& cameraIds,
                                 double exposureMs,
                                 QString* errorMessage)
    {
        if (!m_cameraProvider || cameraIds.isEmpty())
        {
            if (errorMessage) *errorMessage = QStringLiteral("Camera provider not available");
            return false;
        }
        for (const QString& cameraId : cameraIds)
        {
            if (!m_cameraProvider->setExposure(cameraId, exposureMs))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Failed to set exposure for camera: %1")
                                        .arg(cameraId);
                }
                return false;
            }
        }
        return true;
    }

    // Moves the active XY stage to an event position
    bool MDAManager::moveXY(double x, double y, QString* errorMessage)
    {
        if (!m_stageProvider)
        {
            if (errorMessage) *errorMessage = QStringLiteral("Stage provider not available");
            return false;
        }
        const QString stage = m_stageProvider->defaultXYStage();
        if (stage.isEmpty())
        {
            if (errorMessage) *errorMessage = QStringLiteral("No XY stage device configured");
            return false;
        }
        return m_stageProvider->setXYPosition(stage, x, y, errorMessage);
    }

    // Moves the active focus device to an event position
    bool MDAManager::moveZ(double z, QString* errorMessage)
    {
        if (!m_stageProvider)
        {
            if (errorMessage) *errorMessage = QStringLiteral("Stage provider not available");
            return false;
        }
        const QString focus = m_stageProvider->defaultZStage();
        if (focus.isEmpty())
        {
            if (errorMessage) *errorMessage = QStringLiteral("No focus device configured");
            return false;
        }
        return m_stageProvider->setZPosition(focus, z, errorMessage);
    }
} // namespace scopeone::core::internal

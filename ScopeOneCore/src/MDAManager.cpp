#include "internal/MDAManager.h"

#include "internal/CameraManager.h"
#include "MMCore.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <future>
#include <limits>
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
    MDAManager::MDAManager(std::shared_ptr<CMMCore> core, QObject* parent)
        : QObject(parent)
        , m_mmcore(std::move(core))
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
    void MDAManager::setCameraManager(CameraManager* cameraManager)
    {
        m_cameraManager = cameraManager;
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
        if (!m_mmcore)
        {
            m_running.store(false);
            emit sequenceError(QStringLiteral("MMCore not available"));
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
        if (!m_mmcore)
        {
            if (errorMessage) *errorMessage = QStringLiteral("MMCore not available");
            return false;
        }

        if (event.exposureMs > 0.0 && !setExposure(event.exposureMs, errorMessage))
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

        try
        {
            m_mmcore->waitForSystem();
        }
        catch (const CMMError& e)
        {
            if (errorMessage) *errorMessage = QString::fromStdString(e.getMsg());
            return false;
        }
        return true;
    }

    // Routes one event to the active capture implementation
    bool MDAManager::execEvent(const AcquisitionEvent& event, MDAOutput& output, QString* errorMessage)
    {
        if (event.cameraIds.size() > 1)
        {
            if (!m_cameraManager)
            {
                if (errorMessage) *errorMessage = QStringLiteral("CameraManager not available");
                return false;
            }
            return execEventMultiCamera(event, output, errorMessage);
        }
        return execEventSingleCamera(event, output, errorMessage);
    }

    // Captures one event through the native MMCore camera path
    bool MDAManager::execEventSingleCamera(const AcquisitionEvent& event,
                                           MDAOutput& output,
                                           QString* errorMessage)
    {
        if (!m_mmcore)
        {
            if (errorMessage) *errorMessage = QStringLiteral("MMCore not available");
            return false;
        }

        try
        {
            m_mmcore->snapImage();

            const unsigned width = m_mmcore->getImageWidth();
            const unsigned height = m_mmcore->getImageHeight();
            const unsigned bytesPerPixel = m_mmcore->getBytesPerPixel();
            if (bytesPerPixel != 1 && bytesPerPixel != 2)
            {
                if (errorMessage) *errorMessage = QStringLiteral("Unsupported pixel format");
                return false;
            }

            const qint64 stride = static_cast<qint64>(width) * bytesPerPixel;
            const qint64 byteCount = stride * height;
            if (width > static_cast<unsigned>((std::numeric_limits<int>::max)())
                || height > static_cast<unsigned>((std::numeric_limits<int>::max)())
                || stride > (std::numeric_limits<int>::max)()
                || byteCount <= 0
                || byteCount > (std::numeric_limits<qsizetype>::max)())
            {
                if (errorMessage) *errorMessage = QStringLiteral("Image frame is too large");
                return false;
            }

            const unsigned char* ptr = static_cast<unsigned char*>(m_mmcore->getImage());
            if (!ptr)
            {
                if (errorMessage) *errorMessage = QStringLiteral("Empty image buffer");
                return false;
            }

            ImageFrame frame;
            frame.cameraId = event.cameraIds.isEmpty() ? QString() : event.cameraIds.first();
            frame.width = static_cast<int>(width);
            frame.height = static_cast<int>(height);
            frame.stride = static_cast<int>(stride);
            frame.pixelFormat = bytesPerPixel == 2 ? ImagePixelFormat::Mono16 : ImagePixelFormat::Mono8;
            frame.bitsPerSample = ImageFrame::normalizedBitsPerSample(
                frame.pixelFormat,
                static_cast<int>(m_mmcore->getImageBitDepth()));
            frame.timestampNs = currentTimestampNs();
            frame.sourceRoiWidth = frame.width;
            frame.sourceRoiHeight = frame.height;
            if (!frame.cameraId.isEmpty())
            {
                try
                {
                    m_mmcore->getROI(frame.cameraId.toStdString().c_str(),
                                     frame.sourceRoiX,
                                     frame.sourceRoiY,
                                     frame.sourceRoiWidth,
                                     frame.sourceRoiHeight);
                }
                catch (const CMMError&)
                {
                    frame.sourceRoiX = 0;
                    frame.sourceRoiY = 0;
                    frame.sourceRoiWidth = frame.width;
                    frame.sourceRoiHeight = frame.height;
                }
            }
            frame.bytes = QByteArray(reinterpret_cast<const char*>(ptr), static_cast<qsizetype>(byteCount));
            output.frames.insert(frame.cameraId, frame);
            return true;
        }
        catch (const CMMError& e)
        {
            if (errorMessage) *errorMessage = QString::fromStdString(e.getMsg());
            return false;
        }
    }

    // Captures one event across active camera processes
    bool MDAManager::execEventMultiCamera(const AcquisitionEvent& event,
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
                if (!m_cameraManager->captureEventFrame(cameraId, result.frame, captureTimeoutMs))
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
    bool MDAManager::setExposure(double exposureMs, QString* errorMessage)
    {
        try
        {
            m_mmcore->setExposure(exposureMs);
            return true;
        }
        catch (const CMMError& e)
        {
            if (errorMessage) *errorMessage = QString::fromStdString(e.getMsg());
            return false;
        }
    }

    // Moves the active XY stage to an event position
    bool MDAManager::moveXY(double x, double y, QString* errorMessage)
    {
        try
        {
            const std::string stage = m_mmcore->getXYStageDevice();
            if (stage.empty())
            {
                if (errorMessage) *errorMessage = QStringLiteral("No XY stage device configured");
                return false;
            }
            m_mmcore->setXYPosition(stage.c_str(), x, y);
            return true;
        }
        catch (const CMMError& e)
        {
            if (errorMessage) *errorMessage = QString::fromStdString(e.getMsg());
            return false;
        }
    }

    // Moves the active focus device to an event position
    bool MDAManager::moveZ(double z, QString* errorMessage)
    {
        try
        {
            const std::string focus = m_mmcore->getFocusDevice();
            if (focus.empty())
            {
                if (errorMessage) *errorMessage = QStringLiteral("No focus device configured");
                return false;
            }
            m_mmcore->setPosition(focus.c_str(), z);
            return true;
        }
        catch (const CMMError& e)
        {
            if (errorMessage) *errorMessage = QString::fromStdString(e.getMsg());
            return false;
        }
    }
} // namespace scopeone::core::internal

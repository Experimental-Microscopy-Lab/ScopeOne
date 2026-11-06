#include "internal/MicroManagerCamera.h"

#include "internal/MicroManagerCalls.h"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QMutexLocker>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace scopeone::core::internal
{
    using micromanager::orElse;

    namespace
    {
        constexpr int kMinimumPollIntervalMs = 1;
        constexpr int kMaximumPollIntervalMs = 50;

        int pollingIntervalFor(double frameIntervalMs)
        {
            if (!std::isfinite(frameIntervalMs) || frameIntervalMs <= 0.0)
            {
                return kMinimumPollIntervalMs;
            }
            return static_cast<int>(std::clamp(frameIntervalMs / 4.0,
                                               static_cast<double>(kMinimumPollIntervalMs),
                                               static_cast<double>(kMaximumPollIntervalMs)));
        }

        bool applyProperties(CMMCore& core,
                             const std::string& label,
                             const QJsonArray& properties,
                             QString* errorMessage)
        {
            for (const QJsonValue& value : properties)
            {
                const QJsonObject property = value.toObject();
                const QString name = property.value(QStringLiteral("name")).toString().trimmed();
                const QString setting = property.value(QStringLiteral("value")).toString();
                try
                {
                    core.setProperty(label.c_str(), name.toStdString().c_str(), setting.toStdString().c_str());
                }
                catch (const CMMError& error)
                {
                    if (errorMessage)
                    {
                        *errorMessage = QStringLiteral("Failed to apply property '%1': %2")
                                            .arg(name, QString::fromStdString(error.getMsg()));
                    }
                    return false;
                }
            }
            return true;
        }

        // The pixel layout of the frames one acquisition delivers
        struct FrameLayout
        {
            QString cameraId;
            int width{0};
            int height{0};
            int stride{0};
            int bitsPerSample{0};
            ImagePixelFormat pixelFormat{ImagePixelFormat::Invalid};
            int sourceRoiX{0};
            int sourceRoiY{0};
            int sourceRoiWidth{0};
            int sourceRoiHeight{0};
            double expectedIntervalMs{0.0};

            qsizetype byteCount() const { return static_cast<qsizetype>(stride) * height; }
        };

        // Reads the layout of the images the camera currently takes
        bool readFrameLayout(CMMCore& core, const QString& cameraId, FrameLayout& layout)
        {
            const std::string camera = cameraId.toStdString();
            const unsigned width = core.getImageWidth();
            const unsigned height = core.getImageHeight();
            const unsigned bytesPerPixel = core.getBytesPerPixel();
            const qint64 stride = static_cast<qint64>(width) * bytesPerPixel;
            if ((bytesPerPixel != 1 && bytesPerPixel != 2)
                || width == 0
                || height == 0
                || width > static_cast<unsigned>((std::numeric_limits<int>::max)())
                || height > static_cast<unsigned>((std::numeric_limits<int>::max)())
                || stride * height > (std::numeric_limits<int>::max)())
            {
                return false;
            }
            layout.cameraId = cameraId;
            layout.width = static_cast<int>(width);
            layout.height = static_cast<int>(height);
            layout.stride = static_cast<int>(stride);
            layout.pixelFormat = bytesPerPixel == 2 ? ImagePixelFormat::Mono16 : ImagePixelFormat::Mono8;
            layout.bitsPerSample = ImageFrame::normalizedBitsPerSample(
                layout.pixelFormat,
                orElse(static_cast<int>(bytesPerPixel) * 8,
                       [&] { return static_cast<int>(core.getImageBitDepth()); }));
            layout.sourceRoiWidth = layout.width;
            layout.sourceRoiHeight = layout.height;
            try
            {
                core.getROI(camera.c_str(), layout.sourceRoiX, layout.sourceRoiY,
                            layout.sourceRoiWidth, layout.sourceRoiHeight);
            }
            catch (const CMMError&)
            {
                layout.sourceRoiX = 0;
                layout.sourceRoiY = 0;
                layout.sourceRoiWidth = layout.width;
                layout.sourceRoiHeight = layout.height;
            }
            return true;
        }

        ImageFrame frameWithLayout(const FrameLayout& layout, quint64 frameIndex)
        {
            ImageFrame frame;
            frame.cameraId = layout.cameraId;
            frame.width = layout.width;
            frame.height = layout.height;
            frame.stride = layout.stride;
            frame.bitsPerSample = layout.bitsPerSample;
            frame.pixelFormat = layout.pixelFormat;
            frame.frameIndex = frameIndex;
            frame.timestampNs = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;
            frame.sourceRoiX = layout.sourceRoiX;
            frame.sourceRoiY = layout.sourceRoiY;
            frame.sourceRoiWidth = layout.sourceRoiWidth;
            frame.sourceRoiHeight = layout.sourceRoiHeight;
            return frame;
        }
    }

    QJsonObject MicroManagerCameraSetup::toJson() const
    {
        return QJsonObject{{QStringLiteral("cameraId"), cameraId},
                           {QStringLiteral("adapter"), adapter},
                           {QStringLiteral("device"), device},
                           {QStringLiteral("preInitProperties"), preInitProperties},
                           {QStringLiteral("properties"), properties},
                           {QStringLiteral("exposureMs"), exposureMs}};
    }

    MicroManagerCameraSetup MicroManagerCameraSetup::fromJson(const QJsonObject& object)
    {
        MicroManagerCameraSetup setup;
        setup.cameraId = object.value(QStringLiteral("cameraId")).toString();
        setup.adapter = object.value(QStringLiteral("adapter")).toString();
        setup.device = object.value(QStringLiteral("device")).toString();
        setup.preInitProperties = object.value(QStringLiteral("preInitProperties")).toArray();
        setup.properties = object.value(QStringLiteral("properties")).toArray();
        setup.exposureMs = object.value(QStringLiteral("exposureMs")).toDouble();
        return setup;
    }

    // Reads the circular buffer of the core on a thread of its own and hands the frames that the
    // delivery mode asks for to the frame sink
    class MicroManagerCameraPoller final : public QObject
    {
    public:
        explicit MicroManagerCameraPoller(MicroManagerCamera* owner)
            : m_owner(owner)
        {
        }

        void start(const FrameLayout& layout)
        {
            m_layout = layout;
            m_observedIntervalMs = layout.expectedIntervalMs;
            m_deliveryTimer.invalidate();
            m_frameIntervalTimer.start();
            if (!m_timer)
            {
                m_timer = new QTimer(this);
                m_timer->setTimerType(Qt::PreciseTimer);
                connect(m_timer, &QTimer::timeout, this, [this]() { poll(); });
            }
            m_timer->setInterval(pollingIntervalFor(m_observedIntervalMs));
            m_active = true;
            if (!m_paused)
            {
                m_timer->start();
            }
        }

        void stop()
        {
            m_active = false;
            if (m_timer)
            {
                m_timer->stop();
            }
            m_frameIntervalTimer.invalidate();
            m_deliveryTimer.invalidate();
        }

        void setPaused(bool paused)
        {
            m_paused = paused;
            if (!m_active || !m_timer)
            {
                return;
            }
            if (paused)
            {
                m_timer->stop();
                return;
            }
            m_frameIntervalTimer.restart();
            m_timer->start();
        }

        void setMode(FrameDeliveryMode mode)
        {
            m_mode = mode;
            m_deliveryTimer.invalidate();
        }

    private:
        void poll()
        {
            CMMCore& core = *m_owner->m_core;
            try
            {
                long remaining = core.getRemainingImageCount();
                if (remaining <= 0)
                {
                    return;
                }

                MicroManagerCamera::FrameSink sink;
                std::function<bool()> filter;
                {
                    QMutexLocker locker(&m_owner->m_sinkMutex);
                    sink = m_owner->m_frameSink;
                    filter = m_owner->m_frameFilter;
                }
                const bool allFrames = m_mode == FrameDeliveryMode::AllFrames;
                const bool deliverNewest =
                    m_mode == FrameDeliveryMode::LatestOnly
                        ? !filter || filter()
                        : !m_deliveryTimer.isValid()
                              || m_deliveryTimer.elapsed() >= kPreviewFrameIntervalMs;
                const qsizetype byteCount = m_layout.byteCount();
                int frameCount = 0;
                bool delivered = false;
                while (remaining-- > 0)
                {
                    const void* pixels = core.popNextImage();
                    if (!pixels)
                    {
                        break;
                    }
                    const quint64 frameIndex = ++m_owner->m_frameIndex;
                    ++frameCount;
                    if (!sink || (!allFrames && (remaining > 0 || !deliverNewest)))
                    {
                        continue;
                    }
                    ImageFrame frame = frameWithLayout(m_layout, frameIndex);
                    frame.bytes = m_owner->m_lendFrames
                                      ? QByteArray::fromRawData(static_cast<const char*>(pixels), byteCount)
                                      : QByteArray(static_cast<const char*>(pixels), byteCount);
                    sink(frame);
                    delivered = true;
                }
                if (delivered && m_mode == FrameDeliveryMode::PreviewLatest)
                {
                    m_deliveryTimer.restart();
                }
                updatePollingInterval(frameCount);
            }
            catch (const CMMError& error)
            {
                stop();
                m_owner->handleAcquisitionFailure(QString::fromStdString(error.getMsg()));
            }
        }

        // Follows the observed frame interval, so a slow camera is not read in a busy loop
        void updatePollingInterval(int frameCount)
        {
            if (frameCount <= 0 || !m_frameIntervalTimer.isValid())
            {
                return;
            }
            const double elapsedMs = static_cast<double>(m_frameIntervalTimer.nsecsElapsed()) / 1000000.0;
            m_frameIntervalTimer.restart();
            const double measuredIntervalMs = elapsedMs / frameCount;
            if (!std::isfinite(measuredIntervalMs) || measuredIntervalMs <= 0.0)
            {
                return;
            }
            m_observedIntervalMs = m_observedIntervalMs > 0.0
                                       ? 0.75 * m_observedIntervalMs + 0.25 * measuredIntervalMs
                                       : measuredIntervalMs;
            const int intervalMs = pollingIntervalFor(m_observedIntervalMs);
            if (m_timer->interval() != intervalMs)
            {
                m_timer->setInterval(intervalMs);
            }
        }

        MicroManagerCamera* const m_owner;
        QTimer* m_timer{nullptr};
        FrameLayout m_layout;
        FrameDeliveryMode m_mode{FrameDeliveryMode::PreviewLatest};
        QElapsedTimer m_frameIntervalTimer;
        QElapsedTimer m_deliveryTimer;
        double m_observedIntervalMs{0.0};
        bool m_active{false};
        bool m_paused{false};
    };

    MicroManagerCamera::MicroManagerCamera(std::shared_ptr<CMMCore> core, QString cameraId, bool lendFrames)
        : m_core(std::move(core))
          , m_cameraId(std::move(cameraId))
          , m_lendFrames(lendFrames)
    {
        m_pollThread.setObjectName(QStringLiteral("ScopeOneCamera_%1").arg(m_cameraId));
        m_poller = new MicroManagerCameraPoller(this);
        m_poller->moveToThread(&m_pollThread);
        QObject::connect(&m_pollThread, &QThread::finished, m_poller, &QObject::deleteLater);
        m_pollThread.start();
    }

    MicroManagerCamera::~MicroManagerCamera()
    {
        if (m_running)
        {
            stopAcquisition();
        }
        m_pollThread.quit();
        m_pollThread.wait();
    }

    std::shared_ptr<MicroManagerCamera> MicroManagerCamera::load(const MicroManagerCameraSetup& setup,
                                                                 bool lendFrames,
                                                                 QString* errorMessage)
    {
        auto core = std::make_shared<CMMCore>();
        const std::string label = setup.cameraId.toStdString();
        try
        {
            core->loadDevice(label.c_str(), setup.adapter.toStdString().c_str(), setup.device.toStdString().c_str());
            if (!applyProperties(*core, label, setup.preInitProperties, errorMessage))
            {
                return {};
            }
            core->initializeDevice(label.c_str());
            core->setCameraDevice(label.c_str());
            if (!applyProperties(*core, label, setup.properties, errorMessage))
            {
                return {};
            }
            // The core holds only this camera, so its circular buffer can take 2 GB. A machine that
            // cannot reserve that much keeps the default buffer instead of failing the camera.
            micromanager::succeeds(nullptr, [&] { core->setCircularBufferMemoryFootprint(2048); });
            if (setup.exposureMs > 0.0)
            {
                core->setExposure(setup.exposureMs);
            }
            core->waitForDevice(label.c_str());
        }
        catch (const CMMError& error)
        {
            if (errorMessage)
            {
                *errorMessage = QString::fromStdString(error.getMsg());
            }
            return {};
        }
        return std::make_shared<MicroManagerCamera>(std::move(core), setup.cameraId, lendFrames);
    }

    HardwareProviderDescriptor MicroManagerCamera::descriptor() const
    {
        return micromanager::descriptor();
    }

    QList<HardwareDeviceDescriptor> MicroManagerCamera::devices() const
    {
        HardwareDeviceDescriptor device;
        device.logicalId = m_cameraId;
        device.providerId = QStringLiteral("micro-manager");
        device.providerDeviceId = m_cameraId;
        device.name = m_cameraId;
        device.kind = HardwareDeviceKind::Camera;
        device.state = HardwareDeviceState::Initialized;
        device.hardwareId = orElse(QString{}, [&]
        {
            return QString::fromStdString(m_core->getDeviceName(m_cameraId.toStdString().c_str()));
        });
        return {device};
    }

    void MicroManagerCamera::setFrameSink(FrameSink sink)
    {
        QMutexLocker locker(&m_sinkMutex);
        m_frameSink = std::move(sink);
    }

    void MicroManagerCamera::setPreviewStateSink(PreviewStateSink sink)
    {
        QMutexLocker locker(&m_sinkMutex);
        m_previewStateSink = std::move(sink);
    }

    void MicroManagerCamera::setErrorSink(std::function<void(const QString&)> sink)
    {
        QMutexLocker locker(&m_sinkMutex);
        m_errorSink = std::move(sink);
    }

    void MicroManagerCamera::setFrameFilter(std::function<bool()> filter)
    {
        QMutexLocker locker(&m_sinkMutex);
        m_frameFilter = std::move(filter);
    }

    // Runs on the poll thread, so a lent frame of the old mode is no longer in use when it returns
    void MicroManagerCamera::setFrameDeliveryMode(FrameDeliveryMode mode)
    {
        MicroManagerCameraPoller* const poller = m_poller;
        QMetaObject::invokeMethod(poller, [poller, mode]() { poller->setMode(mode); },
                                  Qt::BlockingQueuedConnection);
    }

    void MicroManagerCamera::setFrameDeliveryPaused(bool paused)
    {
        if (m_paused == paused)
        {
            return;
        }
        m_paused = paused;
        if (!paused && m_suspended.exchange(false) && m_running)
        {
            try
            {
                if (!m_core->isSequenceRunning())
                {
                    m_core->setCameraDevice(m_cameraId.toStdString().c_str());
                    m_core->startContinuousSequenceAcquisition(0.0);
                }
            }
            catch (const CMMError& error)
            {
                qWarning().noquote() << QStringLiteral("Failed to resume the live view of '%1': %2")
                                            .arg(m_cameraId, QString::fromStdString(error.getMsg()));
            }
        }
        MicroManagerCameraPoller* const poller = m_poller;
        QMetaObject::invokeMethod(poller, [poller, paused]() { poller->setPaused(paused); },
                                  Qt::BlockingQueuedConnection);
    }

    bool MicroManagerCamera::matches(const QString& cameraId) const
    {
        return cameraId.trimmed() == m_cameraId;
    }

    bool MicroManagerCamera::startPreviewFor(const QString& cameraId)
    {
        if (!matches(cameraId))
        {
            return false;
        }
        if (m_running)
        {
            return true;
        }
        if (!startAcquisition())
        {
            return false;
        }
        setPreviewRunning(true);
        return true;
    }

    bool MicroManagerCamera::stopPreviewFor(const QString& cameraId)
    {
        if (!matches(cameraId))
        {
            return false;
        }
        if (!m_running)
        {
            return true;
        }
        stopAcquisition();
        setPreviewRunning(false);
        return true;
    }

    bool MicroManagerCamera::isPreviewRunning(const QString& cameraId) const
    {
        return matches(cameraId) && m_running;
    }

    void MicroManagerCamera::setPreviewRunning(bool running)
    {
        m_running = running;
        PreviewStateSink sink;
        {
            QMutexLocker locker(&m_sinkMutex);
            sink = m_previewStateSink;
        }
        if (sink)
        {
            sink(running);
        }
    }

    bool MicroManagerCamera::startAcquisition()
    {
        FrameLayout layout;
        try
        {
            const std::string camera = m_cameraId.toStdString();
            m_core->setCameraDevice(camera.c_str());
            if (m_core->isSequenceRunning())
            {
                m_core->stopSequenceAcquisition();
            }
            if (!readFrameLayout(*m_core, m_cameraId, layout))
            {
                qWarning().noquote() << QStringLiteral("Camera '%1' delivers a pixel format other than Mono8 or Mono16")
                                            .arg(m_cameraId);
                return false;
            }
            layout.expectedIntervalMs = orElse(0.0, [&] { return m_core->getExposure(); });
            const double actualIntervalMs = orElse(0.0, [&]
            {
                if (!m_core->hasProperty(camera.c_str(), MM::g_Keyword_ActualInterval_ms))
                {
                    return 0.0;
                }
                return std::strtod(m_core->getProperty(camera.c_str(), MM::g_Keyword_ActualInterval_ms).c_str(),
                                   nullptr);
            });
            if (std::isfinite(actualIntervalMs) && actualIntervalMs > layout.expectedIntervalMs)
            {
                layout.expectedIntervalMs = actualIntervalMs;
            }
            m_suspended = false;
            m_core->startContinuousSequenceAcquisition(0.0);
        }
        catch (const CMMError& error)
        {
            qWarning().noquote() << QStringLiteral("Failed to start the live view of '%1': %2")
                                        .arg(m_cameraId, QString::fromStdString(error.getMsg()));
            return false;
        }
        MicroManagerCameraPoller* const poller = m_poller;
        QMetaObject::invokeMethod(poller, [poller, layout]() { poller->start(layout); },
                                  Qt::BlockingQueuedConnection);
        return true;
    }

    void MicroManagerCamera::stopAcquisition()
    {
        MicroManagerCameraPoller* const poller = m_poller;
        QMetaObject::invokeMethod(poller, [poller]() { poller->stop(); }, Qt::BlockingQueuedConnection);
        m_suspended = false;
        try
        {
            if (m_core->isSequenceRunning())
            {
                m_core->stopSequenceAcquisition();
            }
        }
        catch (const CMMError&)
        {
        }
    }

    // Called on the poll thread, which has already stopped polling
    void MicroManagerCamera::handleAcquisitionFailure(const QString& error)
    {
        const QString message = QStringLiteral("The live view of '%1' failed: %2").arg(m_cameraId, error);
        qWarning().noquote() << message;
        try
        {
            if (m_core->isSequenceRunning())
            {
                m_core->stopSequenceAcquisition();
            }
        }
        catch (const CMMError&)
        {
        }
        setPreviewRunning(false);
        std::function<void(const QString&)> sink;
        {
            QMutexLocker locker(&m_sinkMutex);
            sink = m_errorSink;
        }
        if (sink)
        {
            sink(message);
        }
    }

    // A running live acquisition holds images taken before the settings of an event were applied,
    // and the camera cannot be selected while it runs
    void MicroManagerCamera::suspendAcquisition()
    {
        if (m_core->isSequenceRunning())
        {
            m_core->stopSequenceAcquisition();
            m_suspended = true;
        }
        m_core->setCameraDevice(m_cameraId.toStdString().c_str());
    }

    bool MicroManagerCamera::makeFrame(const void* pixels, ImageFrame& frame)
    {
        FrameLayout layout;
        if (!pixels || !readFrameLayout(*m_core, m_cameraId, layout))
        {
            return false;
        }
        frame = frameWithLayout(layout, ++m_frameIndex);
        frame.bytes = QByteArray(static_cast<const char*>(pixels), layout.byteCount());
        return frame.isValid();
    }

    bool MicroManagerCamera::captureEventFrame(const QString& cameraId, ImageFrame& frame, int)
    {
        if (!matches(cameraId))
        {
            return false;
        }
        try
        {
            suspendAcquisition();
            m_core->snapImage();
            return makeFrame(m_core->getImage(), frame);
        }
        catch (const CMMError& error)
        {
            qWarning().noquote() << QStringLiteral("Failed to capture an event frame from '%1': %2")
                                        .arg(m_cameraId, QString::fromStdString(error.getMsg()));
            return false;
        }
    }

    bool MicroManagerCamera::supportsEventSequence(const QString& cameraId) const
    {
        return matches(cameraId);
    }

    // Takes a series of images in one sequence acquisition of the camera. The images come from the
    // circular buffer of the core, so the camera times them itself and can trigger a stage that
    // steps through a sequence of positions.
    bool MicroManagerCamera::captureEventSequence(const QString& cameraId,
                                                  int count,
                                                  std::vector<ImageFrame>& frames,
                                                  int timeoutMs)
    {
        frames.clear();
        if (!matches(cameraId) || count < 1)
        {
            return false;
        }
        const std::string camera = m_cameraId.toStdString();
        bool started = false;
        const auto stop = [&]()
        {
            try
            {
                if (started && m_core->isSequenceRunning(camera.c_str()))
                {
                    m_core->stopSequenceAcquisition(camera.c_str());
                }
            }
            catch (const CMMError&)
            {
            }
        };
        const auto popFrame = [&]()
        {
            ImageFrame frame;
            if (!makeFrame(m_core->popNextImage(), frame))
            {
                return false;
            }
            frames.push_back(std::move(frame));
            return true;
        };
        try
        {
            suspendAcquisition();
            m_core->initializeCircularBuffer();
            m_core->clearCircularBuffer();
            m_core->startSequenceAcquisition(count, 0.0, true);
            started = true;

            QElapsedTimer timer;
            timer.start();
            frames.reserve(static_cast<std::size_t>(count));
            while (static_cast<int>(frames.size()) < count)
            {
                if (m_core->getRemainingImageCount() > 0)
                {
                    if (!popFrame())
                    {
                        stop();
                        return false;
                    }
                    continue;
                }
                if (!m_core->isSequenceRunning(camera.c_str()))
                {
                    break;
                }
                if (timer.elapsed() > timeoutMs)
                {
                    qWarning().noquote() << QStringLiteral("Timed out waiting for the image sequence of '%1'")
                                                .arg(m_cameraId);
                    stop();
                    return false;
                }
                QThread::msleep(1);
            }
            // The last images may arrive between the last check and the end of the sequence
            while (static_cast<int>(frames.size()) < count && m_core->getRemainingImageCount() > 0)
            {
                if (!popFrame())
                {
                    stop();
                    return false;
                }
            }
            stop();
            return static_cast<int>(frames.size()) == count;
        }
        catch (const CMMError& error)
        {
            qWarning().noquote() << QStringLiteral("Failed to capture an image sequence from '%1': %2")
                                        .arg(m_cameraId, QString::fromStdString(error.getMsg()));
            stop();
            return false;
        }
    }

    bool MicroManagerCamera::getExposure(const QString& cameraId, double& exposureMs) const
    {
        if (!matches(cameraId))
        {
            return false;
        }
        try
        {
            exposureMs = m_core->getExposure(m_cameraId.toStdString().c_str());
            return exposureMs > 0.0;
        }
        catch (const CMMError&)
        {
            return false;
        }
    }

    // A camera setting changes the images of a running acquisition, so the acquisition is stopped
    // around it and started again
    template <class Change>
    static bool applyStopped(MicroManagerCamera& camera, Change&& change)
    {
        const bool wasRunning = camera.isPreviewRunning(camera.cameraId());
        if (wasRunning && !camera.stopPreviewFor(camera.cameraId()))
        {
            return false;
        }
        const bool ok = change();
        if (wasRunning && !camera.startPreviewFor(camera.cameraId()))
        {
            return false;
        }
        return ok;
    }

    bool MicroManagerCamera::setExposure(const QString& cameraId, double exposureMs)
    {
        if (!matches(cameraId) || exposureMs <= 0.0)
        {
            return false;
        }
        return applyStopped(*this, [&]()
        {
            try
            {
                const std::string camera = m_cameraId.toStdString();
                m_core->setExposure(camera.c_str(), exposureMs);
                m_core->waitForDevice(camera.c_str());
                return true;
            }
            catch (const CMMError&)
            {
                return false;
            }
        });
    }

    bool MicroManagerCamera::setROI(const QString& cameraId, int x, int y, int width, int height)
    {
        if (!matches(cameraId))
        {
            return false;
        }
        return applyStopped(*this, [&]()
        {
            try
            {
                const std::string camera = m_cameraId.toStdString();
                m_core->setROI(camera.c_str(), x, y, width, height);
                m_core->waitForDevice(camera.c_str());
                return true;
            }
            catch (const CMMError& error)
            {
                qWarning().noquote() << QStringLiteral("Failed to set the ROI of '%1': %2")
                                            .arg(m_cameraId, QString::fromStdString(error.getMsg()));
                return false;
            }
        });
    }

    bool MicroManagerCamera::clearROI(const QString& cameraId)
    {
        if (!matches(cameraId))
        {
            return false;
        }
        return applyStopped(*this, [&]()
        {
            try
            {
                const std::string camera = m_cameraId.toStdString();
                m_core->setCameraDevice(camera.c_str());
                m_core->clearROI();
                m_core->waitForDevice(camera.c_str());
                return true;
            }
            catch (const CMMError& error)
            {
                qWarning().noquote() << QStringLiteral("Failed to clear the ROI of '%1': %2")
                                            .arg(m_cameraId, QString::fromStdString(error.getMsg()));
                return false;
            }
        });
    }

    bool MicroManagerCamera::getROI(const QString& cameraId, int& x, int& y, int& width, int& height)
    {
        if (!matches(cameraId))
        {
            return false;
        }
        try
        {
            m_core->getROI(m_cameraId.toStdString().c_str(), x, y, width, height);
            return true;
        }
        catch (const CMMError&)
        {
            return false;
        }
    }

    QStringList MicroManagerCamera::listProperties(const QString& deviceId)
    {
        if (!matches(deviceId))
        {
            return {};
        }
        return orElse(QStringList{}, [&]
        {
            QStringList names;
            for (const std::string& name : m_core->getDevicePropertyNames(m_cameraId.toStdString().c_str()))
            {
                names.append(QString::fromStdString(name));
            }
            return names;
        });
    }

    QString MicroManagerCamera::getProperty(const QString& deviceId, const QString& name, bool fromCache)
    {
        if (!matches(deviceId))
        {
            return {};
        }
        return orElse(QString{}, [&]
        {
            const std::string camera = m_cameraId.toStdString();
            const std::string property = name.trimmed().toStdString();
            return QString::fromStdString(fromCache
                                              ? m_core->getPropertyFromCache(camera.c_str(), property.c_str())
                                              : m_core->getProperty(camera.c_str(), property.c_str()));
        });
    }

    bool MicroManagerCamera::setProperty(const QString& deviceId,
                                         const QString& name,
                                         const QString& value,
                                         QString* errorMessage)
    {
        if (!matches(deviceId))
        {
            return false;
        }
        return applyStopped(*this, [&]()
        {
            try
            {
                const std::string camera = m_cameraId.toStdString();
                m_core->setProperty(camera.c_str(), name.trimmed().toStdString().c_str(), value.toStdString().c_str());
                m_core->waitForDevice(camera.c_str());
                return true;
            }
            catch (const CMMError& error)
            {
                if (errorMessage)
                {
                    *errorMessage = QString::fromStdString(error.getMsg());
                }
                return false;
            }
        });
    }

    long MicroManagerCamera::propertySequenceCapacity(const QString& deviceId, const QString& name)
    {
        return matches(deviceId) ? micromanager::propertySequenceCapacity(*m_core, m_cameraId, name) : 0;
    }

    bool MicroManagerCamera::startPropertySequence(const QString& deviceId,
                                                   const QString& name,
                                                   const QStringList& values,
                                                   QString* errorMessage)
    {
        return matches(deviceId)
            && micromanager::startPropertySequence(*m_core, m_cameraId, name, values, errorMessage);
    }

    void MicroManagerCamera::stopPropertySequence(const QString& deviceId, const QString& name)
    {
        if (matches(deviceId))
        {
            micromanager::stopPropertySequence(*m_core, m_cameraId, name);
        }
    }

    QString MicroManagerCamera::getPropertyType(const QString& deviceId, const QString& name)
    {
        if (!matches(deviceId))
        {
            return QStringLiteral("Unknown");
        }
        return micromanager::propertyType(*m_core, m_cameraId, name);
    }

    bool MicroManagerCamera::isPropertyReadOnly(const QString& deviceId, const QString& name)
    {
        return !matches(deviceId) || orElse(true, [&]
        {
            return m_core->isPropertyReadOnly(m_cameraId.toStdString().c_str(), name.trimmed().toStdString().c_str());
        });
    }

    bool MicroManagerCamera::isPropertyPreInit(const QString& deviceId, const QString& name)
    {
        return matches(deviceId) && orElse(false, [&]
        {
            return m_core->isPropertyPreInit(m_cameraId.toStdString().c_str(), name.trimmed().toStdString().c_str());
        });
    }

    QStringList MicroManagerCamera::getAllowedPropertyValues(const QString& deviceId, const QString& name)
    {
        if (!matches(deviceId))
        {
            return {};
        }
        return orElse(QStringList{}, [&]
        {
            QStringList values;
            for (const std::string& value : m_core->getAllowedPropertyValues(
                     m_cameraId.toStdString().c_str(), name.trimmed().toStdString().c_str()))
            {
                values.append(QString::fromStdString(value));
            }
            return values;
        });
    }

    bool MicroManagerCamera::hasPropertyLimits(const QString& deviceId, const QString& name)
    {
        return matches(deviceId) && orElse(false, [&]
        {
            return m_core->hasPropertyLimits(m_cameraId.toStdString().c_str(), name.trimmed().toStdString().c_str());
        });
    }

    double MicroManagerCamera::getPropertyLowerLimit(const QString& deviceId, const QString& name)
    {
        return !matches(deviceId) ? 0.0 : orElse(0.0, [&]
        {
            return m_core->getPropertyLowerLimit(m_cameraId.toStdString().c_str(), name.trimmed().toStdString().c_str());
        });
    }

    double MicroManagerCamera::getPropertyUpperLimit(const QString& deviceId, const QString& name)
    {
        return !matches(deviceId) ? 0.0 : orElse(0.0, [&]
        {
            return m_core->getPropertyUpperLimit(m_cameraId.toStdString().c_str(), name.trimmed().toStdString().c_str());
        });
    }
}

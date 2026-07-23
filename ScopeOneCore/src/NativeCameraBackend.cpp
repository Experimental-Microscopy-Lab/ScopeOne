#include "internal/CameraBackend.h"

#include "MMCore.h"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace scopeone::core::internal
{
    namespace
    {
        constexpr int kMinimumPollIntervalMs = 1;
        constexpr int kMaximumPollIntervalMs = 50;
        constexpr int kPreviewFrameDeliveryIntervalMs = 16;

        int pollingIntervalFor(double frameIntervalMs)
        {
            if (!std::isfinite(frameIntervalMs) || frameIntervalMs <= 0.0)
            {
                return kMinimumPollIntervalMs;
            }
            const double intervalMs = std::clamp(frameIntervalMs / 4.0,
                                                  static_cast<double>(kMinimumPollIntervalMs),
                                                  static_cast<double>(kMaximumPollIntervalMs));
            return static_cast<int>(intervalMs);
        }

        struct NativeStreamConfiguration
        {
            std::shared_ptr<CMMCore> core;
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
            quint64 firstFrameIndex{0};
            quint64 generation{0};
            double expectedIntervalMs{0.0};

            bool isValid() const
            {
                const qint64 payloadBytes = static_cast<qint64>(stride) * height;
                const int bytesPerPixel = pixelFormat == ImagePixelFormat::Mono16
                                              ? 2
                                              : (pixelFormat == ImagePixelFormat::Mono8 ? 1 : 0);
                const qint64 minimumStride = static_cast<qint64>(width) * bytesPerPixel;
                return core
                    && !cameraId.trimmed().isEmpty()
                    && width > 0
                    && height > 0
                    && bytesPerPixel > 0
                    && stride >= minimumStride
                    && (stride % bytesPerPixel) == 0
                    && bitsPerSample == ImageFrame::normalizedBitsPerSample(pixelFormat, bitsPerSample)
                    && payloadBytes > 0
                    && payloadBytes <= (std::numeric_limits<qsizetype>::max)()
                    && generation > 0;
            }
        };

        class NativeFrameWorker;

        class NativeCameraBackend final : public CameraBackend
        {
        public:
            NativeCameraBackend();
            ~NativeCameraBackend() override;

            Kind kind() const override { return Kind::Native; }
            void shutdown();
            bool configureNativeCamera(const std::shared_ptr<CMMCore>& core,
                                       const QString& cameraId,
                                       double exposureMs) override;

            bool startPreview() override;
            bool stopPreview() override;
            bool startPreviewFor(const QString& cameraId) override;
            bool stopPreviewFor(const QString& cameraId) override;
            bool isPreviewRunning(const QString& cameraId) const override;
            void setFrameDeliveryPaused(bool paused) override;

        protected:
            bool hasRunningCamera() const override { return m_running; }
            bool resolvePrimaryCameraId(const QString& cameraIdOrAll, QString& cameraId) const override;
            QStringList resolveTargetCameraIds(const QString& cameraIdOrAll) const override;
            bool readExposureFor(const QString& cameraId, double& exposureMs) const override;
            bool writeExposureFor(const QString& cameraId, double exposureMs) override;
            QStringList listPropertiesFor(const QString& cameraId) override;
            bool readPropertyDetailsFor(const QString& cameraId,
                                        const QString& name,
                                        bool fromCache,
                                        CameraPropertyReadback& readback) override;
            bool setPropertyFor(const QString& cameraId,
                                const QString& name,
                                const QString& value,
                                QString* errorMessage) override;
            bool setROIFor(const QString& cameraId, int x, int y, int width, int height) override;
            bool clearROIFor(const QString& cameraId) override;
            bool getROIFor(const QString& cameraId,
                           int& x,
                           int& y,
                           int& width,
                           int& height) override;

        private:
            friend class NativeFrameWorker;

            bool startNativeStream();
            bool stopNativeStream();
            void setWorkerPaused(bool paused);
            void submitWorkerFrames(const QList<ImageFrame>& frames, quint64 acquiredFrameCount);
            bool requiresAllFrames() const;
            bool requiresHighRateFrames() const;
            void handleStreamFailure(quint64 generation,
                                     const QString& cameraId,
                                     const QString& error);
            bool matchesCamera(const QString& cameraId) const;

            std::shared_ptr<CMMCore> m_core;
            QString m_cameraId;
            double m_exposureMs{10.0};
            bool m_running{false};
            bool m_frameDeliveryPaused{false};
            std::atomic<quint64> m_lastFrameIndex{0};
            quint64 m_streamGeneration{0};
            quint64 m_activeGeneration{0};
            QThread m_streamThread;
            NativeFrameWorker* m_worker{nullptr};
        };

        class NativeFrameWorker final : public QObject
        {
        public:
            explicit NativeFrameWorker(NativeCameraBackend* owner)
                : m_owner(owner)
            {
            }

            bool start(const NativeStreamConfiguration& configuration)
            {
                stop();
                if (!configuration.isValid())
                {
                    return false;
                }

                m_configuration = configuration;
                m_configuration.cameraId = m_configuration.cameraId.trimmed();
                m_frameIndex = configuration.firstFrameIndex;
                m_observedIntervalMs = configuration.expectedIntervalMs;
                m_pendingAcquiredFrameCount = 0;
                m_paused = false;
                m_frameIntervalTimer.start();
                m_deliveryTimer.invalidate();

                if (!m_timer)
                {
                    m_timer = new QTimer(this);
                    m_timer->setTimerType(Qt::PreciseTimer);
                    connect(m_timer, &QTimer::timeout, this, [this]() { poll(); });
                }
                m_timer->setInterval(pollingIntervalFor(configuration.expectedIntervalMs));
                m_timer->start();
                return true;
            }

            void stop()
            {
                if (m_timer)
                {
                    m_timer->stop();
                }
                m_configuration = NativeStreamConfiguration{};
                m_frameIntervalTimer.invalidate();
                m_deliveryTimer.invalidate();
                m_frameIndex = 0;
                m_pendingAcquiredFrameCount = 0;
                m_observedIntervalMs = 0.0;
                m_paused = false;
            }

            void setPaused(bool paused)
            {
                if (m_paused == paused || !m_configuration.isValid())
                {
                    return;
                }
                m_paused = paused;
                if (m_paused)
                {
                    m_timer->stop();
                    return;
                }
                m_frameIntervalTimer.restart();
                m_timer->start();
            }

        private:
            void poll()
            {
                if (m_paused || !m_configuration.isValid())
                {
                    return;
                }

                try
                {
                    long remaining = m_configuration.core->getRemainingImageCount();
                    if (remaining <= 0)
                    {
                        return;
                    }

                    const qint64 payloadByteCount = static_cast<qint64>(m_configuration.stride)
                                                    * m_configuration.height;
                    const QPointer<NativeCameraBackend> owner = m_owner;
                    if (!owner)
                    {
                        return;
                    }

                    const bool requiresAllFrames = owner->requiresAllFrames();
                    const bool deliverLatest = requiresAllFrames
                        || owner->requiresHighRateFrames()
                        || !m_deliveryTimer.isValid()
                        || m_deliveryTimer.elapsed() >= kPreviewFrameDeliveryIntervalMs;
                    QList<ImageFrame> frames;
                    int frameCount = 0;
                    while (remaining-- > 0)
                    {
                        const void* pixels = m_configuration.core->popNextImage();
                        if (!pixels)
                        {
                            break;
                        }
                        const quint64 frameIndex = ++m_frameIndex;
                        ++frameCount;
                        if (!requiresAllFrames && (remaining > 0 || !deliverLatest))
                        {
                            continue;
                        }

                        ImageFrame frame;
                        frame.cameraId = m_configuration.cameraId;
                        frame.width = m_configuration.width;
                        frame.height = m_configuration.height;
                        frame.stride = m_configuration.stride;
                        frame.bitsPerSample = m_configuration.bitsPerSample;
                        frame.pixelFormat = m_configuration.pixelFormat;
                        frame.frameIndex = frameIndex;
                        frame.timestampNs = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;
                        frame.sourceRoiX = m_configuration.sourceRoiX;
                        frame.sourceRoiY = m_configuration.sourceRoiY;
                        frame.sourceRoiWidth = m_configuration.sourceRoiWidth;
                        frame.sourceRoiHeight = m_configuration.sourceRoiHeight;
                        frame.bytes.resize(static_cast<qsizetype>(payloadByteCount));
                        memcpy(frame.bytes.data(), pixels, static_cast<size_t>(payloadByteCount));
                        frames.append(std::move(frame));
                    }
                    if (requiresAllFrames)
                    {
                        m_pendingAcquiredFrameCount = 0;
                        if (!frames.isEmpty())
                        {
                            owner->submitWorkerFrames(frames,
                                                      static_cast<quint64>(frameCount));
                        }
                    }
                    else
                    {
                        m_pendingAcquiredFrameCount += static_cast<quint64>(frameCount);
                        if (!frames.isEmpty())
                        {
                            const quint64 acquiredFrameCount = m_pendingAcquiredFrameCount;
                            m_pendingAcquiredFrameCount = 0;
                            m_deliveryTimer.restart();
                            owner->submitWorkerFrames(frames, acquiredFrameCount);
                        }
                    }
                    updatePollingInterval(frameCount);
                }
                catch (const CMMError& error)
                {
                    m_timer->stop();
                    postFailure(m_configuration.generation,
                                m_configuration.cameraId,
                                QString::fromStdString(error.getMsg()));
                }
            }

            void postFailure(quint64 generation, const QString& cameraId, const QString& error)
            {
                const QPointer<NativeCameraBackend> owner = m_owner;
                if (!owner)
                {
                    return;
                }
                QMetaObject::invokeMethod(
                    owner.data(),
                    [owner, generation, cameraId, error]()
                    {
                        if (owner)
                        {
                            owner->handleStreamFailure(generation, cameraId, error);
                        }
                    },
                    Qt::QueuedConnection);
            }

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

            QPointer<NativeCameraBackend> m_owner;
            QTimer* m_timer{nullptr};
            NativeStreamConfiguration m_configuration;
            QElapsedTimer m_frameIntervalTimer;
            QElapsedTimer m_deliveryTimer;
            quint64 m_frameIndex{0};
            quint64 m_pendingAcquiredFrameCount{0};
            double m_observedIntervalMs{0.0};
            bool m_paused{false};
        };

        NativeCameraBackend::NativeCameraBackend()
        {
            m_streamThread.setObjectName(QStringLiteral("ScopeOneNativeFrameWorker"));
            m_worker = new NativeFrameWorker(this);
            m_worker->moveToThread(&m_streamThread);
            connect(&m_streamThread, &QThread::finished, m_worker, &QObject::deleteLater);
            m_streamThread.start();
        }

        NativeCameraBackend::~NativeCameraBackend()
        {
            shutdown();
            m_streamThread.quit();
            m_streamThread.wait();
            m_worker = nullptr;
        }

        void NativeCameraBackend::shutdown()
        {
            if (m_running)
            {
                stopNativeStream();
            }
            m_core.reset();
            m_cameraId.clear();
            m_exposureMs = 10.0;
            m_lastFrameIndex.store(0, std::memory_order_relaxed);
            m_activeGeneration = 0;
            m_frameDeliveryPaused = false;
        }

        bool NativeCameraBackend::configureNativeCamera(const std::shared_ptr<CMMCore>& core,
                                                        const QString& cameraId,
                                                        double exposureMs)
        {
            const QString normalizedId = cameraId.trimmed();
            if (!core || normalizedId.isEmpty())
            {
                return false;
            }
            shutdown();
            m_core = core;
            m_cameraId = normalizedId;
            m_exposureMs = exposureMs > 0.0 ? exposureMs : 10.0;
            return true;
        }

        bool NativeCameraBackend::startPreview()
        {
            return startPreviewFor(m_cameraId);
        }

        bool NativeCameraBackend::stopPreview()
        {
            return stopPreviewFor(m_cameraId);
        }

        bool NativeCameraBackend::startPreviewFor(const QString& cameraId)
        {
            if (!matchesCamera(cameraId) || !m_core)
            {
                return false;
            }
            if (m_running)
            {
                return true;
            }
            if (!startNativeStream())
            {
                return false;
            }
            notifyPreviewStarted(m_cameraId);
            return true;
        }

        bool NativeCameraBackend::stopPreviewFor(const QString& cameraId)
        {
            if (!matchesCamera(cameraId) || !m_core)
            {
                return false;
            }
            if (!m_running)
            {
                return true;
            }
            const bool stopped = stopNativeStream();
            notifyPreviewStopped();
            return stopped;
        }

        bool NativeCameraBackend::isPreviewRunning(const QString& cameraId) const
        {
            return matchesCamera(cameraId) && m_running;
        }

        void NativeCameraBackend::setFrameDeliveryPaused(bool paused)
        {
            if (m_frameDeliveryPaused == paused)
            {
                return;
            }
            m_frameDeliveryPaused = paused;
            setWorkerPaused(paused);
        }

        bool NativeCameraBackend::resolvePrimaryCameraId(const QString& cameraIdOrAll, QString& cameraId) const
        {
            const QString target = cameraIdOrAll.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0
                                       ? m_cameraId
                                       : cameraIdOrAll;
            if (!matchesCamera(target))
            {
                return false;
            }
            cameraId = m_cameraId;
            return true;
        }

        QStringList NativeCameraBackend::resolveTargetCameraIds(const QString& cameraIdOrAll) const
        {
            QString cameraId;
            return resolvePrimaryCameraId(cameraIdOrAll, cameraId) ? QStringList{cameraId} : QStringList{};
        }

        bool NativeCameraBackend::readExposureFor(const QString& cameraId, double& exposureMs) const
        {
            if (!matchesCamera(cameraId) || !m_core)
            {
                return false;
            }
            try
            {
                exposureMs = m_core->getExposure(cameraId.toStdString().c_str());
                return true;
            }
            catch (const CMMError&)
            {
                exposureMs = m_exposureMs;
                return exposureMs > 0.0;
            }
        }

        bool NativeCameraBackend::writeExposureFor(const QString& cameraId, double exposureMs)
        {
            if (!matchesCamera(cameraId) || !m_core)
            {
                return false;
            }
            try
            {
                m_core->setCameraDevice(cameraId.toStdString().c_str());
                m_core->setExposure(exposureMs);
                m_core->waitForDevice(cameraId.toStdString().c_str());
                double actualExposureMs = exposureMs;
                try
                {
                    actualExposureMs = m_core->getExposure();
                }
                catch (const CMMError&)
                {
                }
                m_exposureMs = actualExposureMs;
                return true;
            }
            catch (const CMMError&)
            {
                return false;
            }
        }

        QStringList NativeCameraBackend::listPropertiesFor(const QString& cameraId)
        {
            if (!matchesCamera(cameraId) || !m_core)
            {
                return {};
            }
            try
            {
                QStringList properties;
                for (const std::string& name : m_core->getDevicePropertyNames(cameraId.toStdString().c_str()))
                {
                    properties << QString::fromStdString(name);
                }
                return properties;
            }
            catch (const CMMError&)
            {
                return {};
            }
        }

        bool NativeCameraBackend::readPropertyDetailsFor(const QString& cameraId,
                                                         const QString& name,
                                                         bool fromCache,
                                                         CameraPropertyReadback& readback)
        {
            if (!matchesCamera(cameraId) || !m_core)
            {
                return false;
            }

            const std::string camera = cameraId.toStdString();
            const std::string property = name.toStdString();
            try
            {
                readback.value = QString::fromStdString(
                    fromCache
                        ? m_core->getPropertyFromCache(camera.c_str(), property.c_str())
                        : m_core->getProperty(camera.c_str(), property.c_str()));
                try
                {
                    switch (m_core->getPropertyType(camera.c_str(), property.c_str()))
                    {
                    case MM::String: readback.type = QStringLiteral("String"); break;
                    case MM::Float: readback.type = QStringLiteral("Float"); break;
                    case MM::Integer: readback.type = QStringLiteral("Integer"); break;
                    default: readback.type = QStringLiteral("Unknown"); break;
                    }
                }
                catch (const CMMError&)
                {
                }
                try
                {
                    readback.readOnly = m_core->isPropertyReadOnly(camera.c_str(), property.c_str());
                }
                catch (const CMMError&)
                {
                }
                try
                {
                    readback.preInit = m_core->isPropertyPreInit(camera.c_str(), property.c_str());
                }
                catch (const CMMError&)
                {
                }
                try
                {
                    for (const std::string& value : m_core->getAllowedPropertyValues(camera.c_str(), property.c_str()))
                    {
                        readback.allowedValues << QString::fromStdString(value);
                    }
                }
                catch (const CMMError&)
                {
                }
                try
                {
                    readback.hasLimits = m_core->hasPropertyLimits(camera.c_str(), property.c_str());
                    if (readback.hasLimits)
                    {
                        readback.lowerLimit = m_core->getPropertyLowerLimit(camera.c_str(), property.c_str());
                        readback.upperLimit = m_core->getPropertyUpperLimit(camera.c_str(), property.c_str());
                    }
                }
                catch (const CMMError&)
                {
                }
                return true;
            }
            catch (const CMMError&)
            {
                return false;
            }
        }

        bool NativeCameraBackend::setPropertyFor(const QString& cameraId,
                                                 const QString& name,
                                                 const QString& value,
                                                 QString* errorMessage)
        {
            if (!matchesCamera(cameraId) || !m_core)
            {
                return false;
            }
            try
            {
                m_core->setCameraDevice(cameraId.toStdString().c_str());
                m_core->setProperty(cameraId.toStdString().c_str(),
                                    name.toStdString().c_str(),
                                    value.toStdString().c_str());
                m_core->waitForDevice(cameraId.toStdString().c_str());
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
        }

        bool NativeCameraBackend::setROIFor(const QString& cameraId, int x, int y, int width, int height)
        {
            if (!matchesCamera(cameraId) || !m_core)
            {
                return false;
            }
            try
            {
                m_core->setROI(cameraId.toStdString().c_str(), x, y, width, height);
                m_core->waitForDevice(cameraId.toStdString().c_str());
                return true;
            }
            catch (const CMMError& error)
            {
                qWarning().noquote()
                    << QString("Failed to set ROI for '%1': %2")
                       .arg(cameraId, QString::fromStdString(error.getMsg()));
                return false;
            }
        }

        bool NativeCameraBackend::clearROIFor(const QString& cameraId)
        {
            if (!matchesCamera(cameraId) || !m_core)
            {
                return false;
            }
            try
            {
                m_core->setCameraDevice(cameraId.toStdString().c_str());
                m_core->clearROI();
                m_core->waitForDevice(cameraId.toStdString().c_str());
                return true;
            }
            catch (const CMMError& error)
            {
                qWarning().noquote()
                    << QString("Failed to clear ROI for '%1': %2")
                       .arg(cameraId, QString::fromStdString(error.getMsg()));
                return false;
            }
        }

        bool NativeCameraBackend::getROIFor(const QString& cameraId,
                                            int& x,
                                            int& y,
                                            int& width,
                                            int& height)
        {
            if (!matchesCamera(cameraId) || !m_core)
            {
                return false;
            }
            try
            {
                m_core->getROI(cameraId.toStdString().c_str(), x, y, width, height);
                return true;
            }
            catch (const CMMError& error)
            {
                qWarning().noquote()
                    << QString("Failed to get ROI for '%1': %2")
                       .arg(cameraId, QString::fromStdString(error.getMsg()));
                return false;
            }
        }

        bool NativeCameraBackend::startNativeStream()
        {
            if (!m_core || !m_worker || !m_streamThread.isRunning())
            {
                return false;
            }

            try
            {
                m_core->setCameraDevice(m_cameraId.toStdString().c_str());
                if (m_core->isSequenceRunning())
                {
                    m_core->stopSequenceAcquisition();
                }

                const unsigned width = m_core->getImageWidth();
                const unsigned height = m_core->getImageHeight();
                const unsigned bytesPerPixel = m_core->getBytesPerPixel();
                const qint64 stride = static_cast<qint64>(width) * bytesPerPixel;
                const qint64 payloadByteCount = stride * height;
                if ((bytesPerPixel != 1 && bytesPerPixel != 2)
                    || width > static_cast<unsigned>((std::numeric_limits<int>::max)())
                    || height > static_cast<unsigned>((std::numeric_limits<int>::max)())
                    || stride > (std::numeric_limits<int>::max)()
                    || payloadByteCount <= 0
                    || payloadByteCount > (std::numeric_limits<qsizetype>::max)())
                {
                    return false;
                }

                NativeStreamConfiguration configuration;
                configuration.core = m_core;
                configuration.cameraId = m_cameraId;
                configuration.width = static_cast<int>(width);
                configuration.height = static_cast<int>(height);
                configuration.stride = static_cast<int>(stride);
                configuration.pixelFormat = bytesPerPixel == 2
                                                ? ImagePixelFormat::Mono16
                                                : ImagePixelFormat::Mono8;
                int bitsPerSample = bytesPerPixel == 2 ? 16 : 8;
                try
                {
                    bitsPerSample = static_cast<int>(m_core->getImageBitDepth());
                }
                catch (const CMMError&)
                {
                }
                configuration.bitsPerSample = ImageFrame::normalizedBitsPerSample(
                    configuration.pixelFormat,
                    bitsPerSample);
                configuration.sourceRoiWidth = configuration.width;
                configuration.sourceRoiHeight = configuration.height;
                try
                {
                    m_core->getROI(m_cameraId.toStdString().c_str(),
                                   configuration.sourceRoiX,
                                   configuration.sourceRoiY,
                                   configuration.sourceRoiWidth,
                                   configuration.sourceRoiHeight);
                }
                catch (const CMMError&)
                {
                    configuration.sourceRoiX = 0;
                    configuration.sourceRoiY = 0;
                    configuration.sourceRoiWidth = configuration.width;
                    configuration.sourceRoiHeight = configuration.height;
                }

                try
                {
                    const double hardwareExposureMs = m_core->getExposure();
                    if (hardwareExposureMs > 0.0)
                    {
                        m_exposureMs = hardwareExposureMs;
                    }
                }
                catch (const CMMError&)
                {
                }
                configuration.expectedIntervalMs = m_exposureMs;
                try
                {
                    const std::string camera = m_cameraId.toStdString();
                    if (m_core->hasProperty(camera.c_str(), MM::g_Keyword_ActualInterval_ms))
                    {
                        const std::string value =
                            m_core->getProperty(camera.c_str(), MM::g_Keyword_ActualInterval_ms);
                        char* end = nullptr;
                        const double actualIntervalMs = std::strtod(value.c_str(), &end);
                        if (end != value.c_str()
                            && std::isfinite(actualIntervalMs)
                            && actualIntervalMs > 0.0)
                        {
                            configuration.expectedIntervalMs =
                                (std::max)(configuration.expectedIntervalMs, actualIntervalMs);
                        }
                    }
                }
                catch (const CMMError&)
                {
                }

                configuration.firstFrameIndex = m_lastFrameIndex.load(std::memory_order_relaxed);
                configuration.generation = ++m_streamGeneration;
                m_activeGeneration = configuration.generation;

                m_core->startContinuousSequenceAcquisition(0.0);

                bool workerStarted = false;
                NativeFrameWorker* const worker = m_worker;
                const bool paused = m_frameDeliveryPaused;
                const bool invoked = QMetaObject::invokeMethod(
                    worker,
                    [worker, configuration, paused, &workerStarted]()
                    {
                        workerStarted = worker->start(configuration);
                        if (workerStarted && paused)
                        {
                            worker->setPaused(true);
                        }
                    },
                    Qt::BlockingQueuedConnection);
                if (!invoked || !workerStarted)
                {
                    stopNativeStream();
                    return false;
                }

                m_running = true;
                return true;
            }
            catch (const CMMError& error)
            {
                qWarning().noquote()
                    << QString("Failed to start native camera stream for '%1': %2")
                       .arg(m_cameraId, QString::fromStdString(error.getMsg()));
                stopNativeStream();
                return false;
            }
        }

        bool NativeCameraBackend::stopNativeStream()
        {
            ++m_streamGeneration;
            m_activeGeneration = 0;

            bool workerStopped = !m_worker || !m_streamThread.isRunning();
            if (m_worker && m_streamThread.isRunning())
            {
                NativeFrameWorker* const worker = m_worker;
                workerStopped = QMetaObject::invokeMethod(
                    worker,
                    [worker]() { worker->stop(); },
                    Qt::BlockingQueuedConnection);
            }
            discardPendingPreviewFrames();

            bool sequenceStopped = true;
            try
            {
                if (m_core && m_core->isSequenceRunning())
                {
                    m_core->stopSequenceAcquisition();
                }
            }
            catch (const CMMError&)
            {
                sequenceStopped = false;
            }
            m_running = false;
            return workerStopped && sequenceStopped;
        }

        void NativeCameraBackend::setWorkerPaused(bool paused)
        {
            if (!m_worker || !m_streamThread.isRunning())
            {
                return;
            }
            NativeFrameWorker* const worker = m_worker;
            QMetaObject::invokeMethod(
                worker,
                [worker, paused]() { worker->setPaused(paused); },
                Qt::BlockingQueuedConnection);
        }

        void NativeCameraBackend::submitWorkerFrames(const QList<ImageFrame>& frames,
                                                      quint64 acquiredFrameCount)
        {
            if (frames.isEmpty())
            {
                return;
            }
            m_lastFrameIndex.store(frames.constLast().frameIndex, std::memory_order_relaxed);
            submitFrames(frames, acquiredFrameCount);
        }

        bool NativeCameraBackend::requiresAllFrames() const
        {
            return recordingFrameDeliveryEnabled();
        }

        bool NativeCameraBackend::requiresHighRateFrames() const
        {
            return highRateFrameDeliveryEnabled();
        }

        void NativeCameraBackend::handleStreamFailure(quint64 generation,
                                                      const QString& cameraId,
                                                      const QString& error)
        {
            if (generation != m_activeGeneration || cameraId != m_cameraId)
            {
                return;
            }
            const QString message = QStringLiteral("Native camera stream failed for '%1': %2")
                                        .arg(cameraId, error);
            const bool wasRecording = recordingFrameDeliveryEnabled();
            if (wasRecording)
            {
                CameraBackend::setRecordingFrameDeliveryEnabled(false);
            }
            qWarning().noquote() << message;
            stopNativeStream();
            notifyPreviewStopped();
            if (wasRecording)
            {
                emit frameDeliveryFailed(message);
            }
        }

        bool NativeCameraBackend::matchesCamera(const QString& cameraId) const
        {
            return !m_cameraId.isEmpty() && cameraId.trimmed() == m_cameraId;
        }
    }

    std::unique_ptr<CameraBackend> createNativeCameraBackend()
    {
        return std::make_unique<NativeCameraBackend>();
    }
}

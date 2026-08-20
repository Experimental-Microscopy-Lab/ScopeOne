#include "scopeone/SimulatorProvider.h"

#include <QDateTime>
#include <QMutexLocker>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <utility>

namespace scopeone::core
{
    SimulatorProvider::SimulatorProvider(const QString& logicalCameraId,
                                         int width,
                                         int height,
                                         const QString& providerId)
        : m_providerId(providerId.trimmed().isEmpty()
                           ? QStringLiteral("simulator.%1").arg(
                                 QUuid::createUuid().toString(QUuid::WithoutBraces))
                           : providerId.trimmed())
          , m_cameraId(logicalCameraId.trimmed().isEmpty()
                           ? QStringLiteral("camera.simulator")
                           : logicalCameraId.trimmed())
          , m_sensorWidth((std::max)(1, width))
          , m_sensorHeight((std::max)(1, height))
          , m_roi(0, 0, m_sensorWidth, m_sensorHeight)
    {
        m_timer.setTimerType(Qt::PreciseTimer);
        updateTimerInterval();
        connect(&m_timer, &QTimer::timeout, this, [this]()
        {
            FrameSink sink;
            {
                QMutexLocker locker(&m_mutex);
                sink = m_frameSink;
            }
            if (sink) sink(makeFrame());
        });
    }

    HardwareProviderDescriptor SimulatorProvider::descriptor() const
    {
        return {m_providerId, QStringLiteral("ScopeOne Simulator"), QStringLiteral("1")};
    }

    QList<HardwareDeviceDescriptor> SimulatorProvider::devices() const
    {
        HardwareDeviceDescriptor camera;
        camera.logicalId = m_cameraId;
        camera.providerId = m_providerId;
        camera.providerDeviceId = m_cameraId;
        camera.hardwareId = m_cameraId;
        camera.name = QStringLiteral("Simulator Camera");
        camera.kind = HardwareDeviceKind::Camera;
        camera.state = HardwareDeviceState::Initialized;
        camera.endpoint = HardwareEndpointKind::InProcess;
        return {camera};
    }

    void SimulatorProvider::setFrameSink(FrameSink sink)
    {
        QMutexLocker locker(&m_mutex);
        m_frameSink = std::move(sink);
    }

    void SimulatorProvider::setPreviewStateSink(PreviewStateSink sink)
    {
        QMutexLocker locker(&m_mutex);
        m_previewStateSink = std::move(sink);
    }

    bool SimulatorProvider::startPreview()
    {
        {
            QMutexLocker locker(&m_mutex);
            if (!m_frameSink) return false;
        }
        m_timer.start();
        PreviewStateSink sink;
        {
            QMutexLocker locker(&m_mutex);
            sink = m_previewStateSink;
        }
        if (sink) sink(true);
        return true;
    }

    bool SimulatorProvider::stopPreview()
    {
        const bool wasRunning = m_timer.isActive();
        m_timer.stop();
        PreviewStateSink sink;
        {
            QMutexLocker locker(&m_mutex);
            sink = m_previewStateSink;
        }
        if (wasRunning && sink) sink(false);
        return true;
    }

    bool SimulatorProvider::startPreviewFor(const QString& cameraId)
    {
        return accepts(cameraId) && startPreview();
    }

    bool SimulatorProvider::stopPreviewFor(const QString& cameraId)
    {
        return accepts(cameraId) && stopPreview();
    }

    bool SimulatorProvider::isPreviewRunning(const QString& cameraId) const
    {
        return accepts(cameraId) && m_timer.isActive();
    }

    bool SimulatorProvider::getExposure(const QString& cameraIdOrAll, double& exposureMs) const
    {
        if (!accepts(cameraIdOrAll))
        {
            return false;
        }
        QMutexLocker locker(&m_mutex);
        exposureMs = m_exposureMs;
        return true;
    }

    bool SimulatorProvider::setExposure(const QString& cameraIdOrAll, double exposureMs)
    {
        if (!accepts(cameraIdOrAll) || !std::isfinite(exposureMs) || exposureMs <= 0.0)
        {
            return false;
        }
        {
            QMutexLocker locker(&m_mutex);
            m_exposureMs = exposureMs;
        }
        if (QThread::currentThread() == thread())
        {
            updateTimerInterval();
        }
        else
        {
            QMetaObject::invokeMethod(this,
                                      [this]() { updateTimerInterval(); },
                                      Qt::QueuedConnection);
        }
        return true;
    }

    QStringList SimulatorProvider::listProperties(const QString& cameraId)
    {
        return accepts(cameraId)
                   ? QStringList{QStringLiteral("Exposure"),
                                 QStringLiteral("SensorWidth"),
                                 QStringLiteral("SensorHeight")}
                   : QStringList{};
    }

    QString SimulatorProvider::getProperty(const QString& cameraId,
                                           const QString& name,
                                           bool)
    {
        if (!accepts(cameraId))
        {
            return {};
        }
        if (name == QStringLiteral("Exposure"))
        {
            QMutexLocker locker(&m_mutex);
            return QString::number(m_exposureMs, 'g', 12);
        }
        if (name == QStringLiteral("SensorWidth"))
        {
            return QString::number(m_sensorWidth);
        }
        if (name == QStringLiteral("SensorHeight"))
        {
            return QString::number(m_sensorHeight);
        }
        return {};
    }

    bool SimulatorProvider::setProperty(const QString& cameraId,
                                        const QString& name,
                                        const QString& value,
                                        QString* errorMessage)
    {
        if (errorMessage)
        {
            errorMessage->clear();
        }
        if (!accepts(cameraId) || name != QStringLiteral("Exposure"))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Property is not writable");
            }
            return false;
        }
        bool ok = false;
        const double exposureMs = value.toDouble(&ok);
        if (!ok || !setExposure(cameraId, exposureMs))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Invalid exposure value");
            }
            return false;
        }
        return true;
    }

    QString SimulatorProvider::getPropertyType(const QString& cameraId, const QString& name)
    {
        return accepts(cameraId) && listProperties(cameraId).contains(name)
                   ? (name == QStringLiteral("Exposure")
                          ? QStringLiteral("Float")
                          : QStringLiteral("Integer"))
                   : QStringLiteral("Unknown");
    }

    bool SimulatorProvider::isPropertyReadOnly(const QString& cameraId, const QString& name)
    {
        return !accepts(cameraId) || name != QStringLiteral("Exposure");
    }

    bool SimulatorProvider::isPropertyPreInit(const QString&, const QString&)
    {
        return false;
    }

    QStringList SimulatorProvider::getAllowedPropertyValues(const QString&, const QString&)
    {
        return {};
    }

    bool SimulatorProvider::hasPropertyLimits(const QString& cameraId, const QString& name)
    {
        return accepts(cameraId) && name == QStringLiteral("Exposure");
    }

    double SimulatorProvider::getPropertyLowerLimit(const QString& cameraId, const QString& name)
    {
        return accepts(cameraId) && name == QStringLiteral("Exposure") ? 0.1 : 0.0;
    }

    double SimulatorProvider::getPropertyUpperLimit(const QString& cameraId, const QString& name)
    {
        return accepts(cameraId) && name == QStringLiteral("Exposure") ? 1000.0 : 0.0;
    }

    bool SimulatorProvider::setROI(const QString& cameraId,
                                   int x,
                                   int y,
                                   int width,
                                   int height)
    {
        const QRect roi(x, y, width, height);
        const QRect sensor(0, 0, m_sensorWidth, m_sensorHeight);
        if (!accepts(cameraId) || width <= 0 || height <= 0 || !sensor.contains(roi))
        {
            return false;
        }
        QMutexLocker locker(&m_mutex);
        m_roi = roi;
        return true;
    }

    bool SimulatorProvider::clearROI(const QString& cameraId)
    {
        if (!accepts(cameraId))
        {
            return false;
        }
        QMutexLocker locker(&m_mutex);
        m_roi = QRect(0, 0, m_sensorWidth, m_sensorHeight);
        return true;
    }

    bool SimulatorProvider::getROI(const QString& cameraId,
                                   int& x,
                                   int& y,
                                   int& width,
                                   int& height)
    {
        if (!accepts(cameraId))
        {
            return false;
        }
        QMutexLocker locker(&m_mutex);
        x = m_roi.x();
        y = m_roi.y();
        width = m_roi.width();
        height = m_roi.height();
        return true;
    }

    bool SimulatorProvider::captureEventFrame(const QString& cameraId,
                                              ImageFrame& frame,
                                              int)
    {
        if (!accepts(cameraId))
        {
            return false;
        }
        frame = makeFrame();
        return frame.isValid();
    }

    bool SimulatorProvider::accepts(const QString& cameraIdOrAll) const
    {
        const QString target = cameraIdOrAll.trimmed();
        return target == m_cameraId
            || target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0;
    }

    ImageFrame SimulatorProvider::makeFrame()
    {
        QMutexLocker locker(&m_mutex);
        ImageFrame frame;
        frame.cameraId = m_cameraId;
        frame.width = m_roi.width();
        frame.height = m_roi.height();
        frame.stride = frame.width;
        frame.bitsPerSample = 8;
        frame.pixelFormat = ImagePixelFormat::Mono8;
        frame.frameIndex = ++m_frameIndex;
        frame.timestampNs = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;
        frame.sourceRoiX = m_roi.x();
        frame.sourceRoiY = m_roi.y();
        frame.sourceRoiWidth = m_roi.width();
        frame.sourceRoiHeight = m_roi.height();
        frame.bytes.resize(static_cast<qsizetype>(frame.width) * frame.height);
        for (int y = 0; y < frame.height; ++y)
        {
            uchar* row = reinterpret_cast<uchar*>(frame.bytes.data())
                + static_cast<qsizetype>(y) * frame.stride;
            for (int x = 0; x < frame.width; ++x)
            {
                row[x] = static_cast<uchar>((x + y + frame.frameIndex) & 0xffu);
            }
        }
        return frame;
    }

    void SimulatorProvider::updateTimerInterval()
    {
        double exposureMs = 0.0;
        {
            QMutexLocker locker(&m_mutex);
            exposureMs = m_exposureMs;
        }
        m_timer.setInterval((std::max)(1, static_cast<int>(std::ceil(exposureMs))));
    }
}

#include "scopeone/SimulatorProvider.h"

#include <QDateTime>
#include <QMutexLocker>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace scopeone::core
{
    namespace
    {
        struct Bead
        {
            double x;
            double y;
            double brightness;
        };

        constexpr std::array<Bead, 32> Beads{{
            {0.08, 0.10, 0.91}, {0.19, 0.08, 0.84}, {0.32, 0.12, 0.98},
            {0.46, 0.07, 0.88}, {0.62, 0.11, 0.94}, {0.77, 0.08, 0.82},
            {0.91, 0.13, 0.96}, {0.13, 0.25, 0.87}, {0.29, 0.29, 1.00},
            {0.48, 0.23, 0.83}, {0.69, 0.27, 0.92}, {0.87, 0.31, 0.86},
            {0.08, 0.43, 0.95}, {0.24, 0.47, 0.81}, {0.42, 0.39, 0.90},
            {0.61, 0.45, 0.97}, {0.80, 0.42, 0.85}, {0.94, 0.50, 0.93},
            {0.14, 0.63, 0.89}, {0.34, 0.58, 0.99}, {0.53, 0.66, 0.84},
            {0.72, 0.61, 0.96}, {0.89, 0.68, 0.80}, {0.07, 0.82, 0.92},
            {0.22, 0.88, 0.86}, {0.39, 0.79, 0.95}, {0.58, 0.86, 0.82},
            {0.76, 0.81, 0.98}, {0.92, 0.89, 0.88}, {0.47, 0.94, 0.91},
            {0.30, 0.70, 0.85}, {0.67, 0.75, 0.94}
        }};

        constexpr std::array<Bead, 8> BeadDoublets{{
            {0.175, 0.176, 1.00}, {0.191, 0.176, 0.92},
            {0.535, 0.335, 0.88}, {0.551, 0.335, 0.96},
            {0.365, 0.515, 0.98}, {0.381, 0.515, 0.86},
            {0.705, 0.925, 0.91}, {0.721, 0.925, 1.00}
        }};
    }

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
                                 QStringLiteral("ImageMode"),
                                 QStringLiteral("Resolution"),
                                 QStringLiteral("GaussianNoiseSigma"),
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
        if (name == QStringLiteral("ImageMode"))
        {
            QMutexLocker locker(&m_mutex);
            switch (m_imageMode)
            {
            case ImageMode::Gradient: return QStringLiteral("Gradient");
            case ImageMode::Hologram: return QStringLiteral("Hologram");
            case ImageMode::Beads: return QStringLiteral("Fluorescent Beads");
            }
        }
        if (name == QStringLiteral("Resolution"))
        {
            QMutexLocker locker(&m_mutex);
            return QString::number(m_sensorWidth);
        }
        if (name == QStringLiteral("GaussianNoiseSigma"))
        {
            QMutexLocker locker(&m_mutex);
            return QString::number(m_gaussianNoiseSigma, 'g', 12);
        }
        if (name == QStringLiteral("SensorWidth"))
        {
            QMutexLocker locker(&m_mutex);
            return QString::number(m_sensorWidth);
        }
        if (name == QStringLiteral("SensorHeight"))
        {
            QMutexLocker locker(&m_mutex);
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
        if (!accepts(cameraId))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Property is not writable");
            }
            return false;
        }
        if (name == QStringLiteral("ImageMode"))
        {
            const QString mode = value.trimmed();
            if (mode != QStringLiteral("Gradient")
                && mode != QStringLiteral("Hologram")
                && mode != QStringLiteral("Fluorescent Beads"))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Invalid image mode");
                }
                return false;
            }
            QMutexLocker locker(&m_mutex);
            m_imageMode = mode == QStringLiteral("Hologram") ? ImageMode::Hologram
                        : mode == QStringLiteral("Fluorescent Beads") ? ImageMode::Beads
                        : ImageMode::Gradient;
            if (m_imageMode == ImageMode::Beads && m_beadsImage.isEmpty())
            {
                rebuildBeadsImage();
            }
            return true;
        }
        if (name == QStringLiteral("Resolution"))
        {
            const int resolution = value.toInt();
            if (!getAllowedPropertyValues(cameraId, name).contains(QString::number(resolution)))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Invalid resolution");
                }
                return false;
            }
            QMutexLocker locker(&m_mutex);
            m_sensorWidth = resolution;
            m_sensorHeight = resolution;
            m_roi = QRect(0, 0, resolution, resolution);
            m_beadsImage.clear();
            if (m_imageMode == ImageMode::Beads)
            {
                rebuildBeadsImage();
            }
            return true;
        }
        if (name == QStringLiteral("GaussianNoiseSigma"))
        {
            QMutexLocker locker(&m_mutex);
            m_gaussianNoiseSigma = std::clamp(value.toDouble(), 0.0, 100.0);
            return true;
        }
        if (name == QStringLiteral("Exposure"))
        {
            bool ok = false;
            const double exposureMs = value.toDouble(&ok);
            if (ok && setExposure(cameraId, exposureMs))
            {
                return true;
            }
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Invalid exposure value");
            }
        }
        else if (errorMessage)
        {
            *errorMessage = QStringLiteral("Property is not writable");
        }
        return false;
    }

    QString SimulatorProvider::getPropertyType(const QString& cameraId, const QString& name)
    {
        return accepts(cameraId) && listProperties(cameraId).contains(name)
                   ? (name == QStringLiteral("Exposure")
                              || name == QStringLiteral("GaussianNoiseSigma")
                          ? QStringLiteral("Float")
                          : name == QStringLiteral("ImageMode")
                                ? QStringLiteral("String")
                                : QStringLiteral("Integer"))
                   : QStringLiteral("Unknown");
    }

    bool SimulatorProvider::isPropertyReadOnly(const QString& cameraId, const QString& name)
    {
        return !accepts(cameraId)
            || (name != QStringLiteral("Exposure")
                && name != QStringLiteral("ImageMode")
                && name != QStringLiteral("Resolution")
                && name != QStringLiteral("GaussianNoiseSigma"));
    }

    bool SimulatorProvider::isPropertyPreInit(const QString&, const QString&)
    {
        return false;
    }

    QStringList SimulatorProvider::getAllowedPropertyValues(const QString& cameraId,
                                                             const QString& name)
    {
        if (accepts(cameraId) && name == QStringLiteral("ImageMode"))
        {
            return {QStringLiteral("Gradient"),
                    QStringLiteral("Hologram"),
                    QStringLiteral("Fluorescent Beads")};
        }
        if (accepts(cameraId) && name == QStringLiteral("Resolution"))
        {
            return {QStringLiteral("256"),
                    QStringLiteral("512"),
                    QStringLiteral("1024"),
                    QStringLiteral("2048")};
        }
        return {};
    }

    bool SimulatorProvider::hasPropertyLimits(const QString& cameraId, const QString& name)
    {
        return accepts(cameraId)
            && (name == QStringLiteral("Exposure")
                || name == QStringLiteral("GaussianNoiseSigma"));
    }

    double SimulatorProvider::getPropertyLowerLimit(const QString& cameraId, const QString& name)
    {
        if (!accepts(cameraId))
        {
            return 0.0;
        }
        return name == QStringLiteral("Exposure") ? 0.1 : 0.0;
    }

    double SimulatorProvider::getPropertyUpperLimit(const QString& cameraId, const QString& name)
    {
        if (!accepts(cameraId))
        {
            return 0.0;
        }
        return name == QStringLiteral("Exposure") ? 1000.0
             : name == QStringLiteral("GaussianNoiseSigma") ? 100.0
             : 0.0;
    }

    bool SimulatorProvider::setROI(const QString& cameraId,
                                   int x,
                                   int y,
                                   int width,
                                   int height)
    {
        const QRect roi(x, y, width, height);
        if (!accepts(cameraId) || width <= 0 || height <= 0)
        {
            return false;
        }
        QMutexLocker locker(&m_mutex);
        if (!QRect(0, 0, m_sensorWidth, m_sensorHeight).contains(roi))
        {
            return false;
        }
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
        std::normal_distribution<double> gaussianNoise;
        constexpr double twoPi = 6.28318530717958647692;
        for (int y = 0; y < frame.height; ++y)
        {
            uchar* row = reinterpret_cast<uchar*>(frame.bytes.data())
                + static_cast<qsizetype>(y) * frame.stride;
            for (int x = 0; x < frame.width; ++x)
            {
                double intensity = 0.0;
                if (m_imageMode == ImageMode::Gradient)
                {
                    intensity = static_cast<double>((x + y + frame.frameIndex) & 0xffu);
                }
                else if (m_imageMode == ImageMode::Hologram)
                {
                    const int sensorX = m_roi.x() + x;
                    const int sensorY = m_roi.y() + y;
                    const double nx = (sensorX - 0.5 * m_sensorWidth) / m_sensorWidth;
                    const double ny = (sensorY - 0.5 * m_sensorHeight) / m_sensorHeight;
                    const double objectAmplitude =
                        0.65 * std::exp(-35.0 * (nx * nx + ny * ny))
                        + 0.35 * std::exp(-90.0 * ((nx - 0.18) * (nx - 0.18)
                                                   + (ny + 0.12) * (ny + 0.12)));
                    const double objectPhase = 18.0 * (nx * nx + ny * ny)
                                             + 0.015 * static_cast<double>(frame.frameIndex);
                    const double carrier = twoPi * (48.0 * sensorX / m_sensorWidth
                                                     + 32.0 * sensorY / m_sensorHeight);
                    intensity = 70.0
                              + 55.0 * objectAmplitude * objectAmplitude
                              + 120.0 * objectAmplitude * std::cos(carrier + objectPhase);
                }
                else
                {
                    const int sensorX = m_roi.x() + x;
                    const int sensorY = m_roi.y() + y;
                    intensity = static_cast<uchar>(m_beadsImage.at(
                        sensorY * m_sensorWidth + sensorX));
                }
                if (m_gaussianNoiseSigma > 0.0)
                {
                    intensity += m_gaussianNoiseSigma * gaussianNoise(m_randomGenerator);
                }
                row[x] = static_cast<uchar>(std::clamp(intensity, 0.0, 255.0));
            }
        }
        return frame;
    }

    // Build a static field of Gaussian PSFs for fast live preview
    void SimulatorProvider::rebuildBeadsImage()
    {
        std::vector<float> intensities(
            static_cast<std::size_t>(m_sensorWidth) * m_sensorHeight, 3.0f);
        const double resolutionScale = (std::min)(m_sensorWidth, m_sensorHeight) / 256.0;
        const double sigma = 4.0 * resolutionScale / 2.354820045;
        const int radius = static_cast<int>(std::ceil(4.0 * sigma));

        const auto drawBead = [&](const Bead& bead)
        {
            const double centerX = bead.x * m_sensorWidth;
            const double centerY = bead.y * m_sensorHeight;
            const int minX = (std::max)(0, static_cast<int>(std::floor(centerX)) - radius);
            const int maxX = (std::min)(m_sensorWidth - 1,
                                        static_cast<int>(std::floor(centerX)) + radius);
            const int minY = (std::max)(0, static_cast<int>(std::floor(centerY)) - radius);
            const int maxY = (std::min)(m_sensorHeight - 1,
                                        static_cast<int>(std::floor(centerY)) + radius);
            const double denominator = 2.0 * sigma * sigma;
            for (int y = minY; y <= maxY; ++y)
            {
                for (int x = minX; x <= maxX; ++x)
                {
                    const double dx = x + 0.5 - centerX;
                    const double dy = y + 0.5 - centerY;
                    intensities[static_cast<std::size_t>(y) * m_sensorWidth + x]
                        += static_cast<float>(245.0 * bead.brightness
                                             * std::exp(-(dx * dx + dy * dy) / denominator));
                }
            }
        };

        for (const Bead& bead : Beads)
        {
            drawBead(bead);
        }
        for (const Bead& bead : BeadDoublets)
        {
            drawBead(bead);
        }

        m_beadsImage.resize(m_sensorWidth * m_sensorHeight);
        for (qsizetype i = 0; i < m_beadsImage.size(); ++i)
        {
            m_beadsImage[i] = static_cast<char>(std::clamp(intensities[i], 0.0f, 255.0f));
        }
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

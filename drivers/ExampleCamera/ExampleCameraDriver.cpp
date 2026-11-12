// A driver written the way a camera maker would write one. It uses only the headers of
// scopeone::DriverSDK. Where a real driver calls its vendor SDK, this one renders the test
// patterns of TestPatterns.h on an acquisition thread, as a vendor SDK delivers frames from a
// thread of its own.
#include "TestPatterns.h"

#include "scopeone/DriverSDK.h"

#include <QMutex>
#include <QMutexLocker>
#include <QObject>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace example_camera
{
    using scopeone::core::Camera;
    using scopeone::core::Device;
    using scopeone::core::HardwareProviderDescriptor;
    using scopeone::core::ImageFrame;
    using scopeone::core::ImagePixelFormat;

    constexpr int kBitsPerSample = 12;
    constexpr double kMinimumExposureMs = 1.0;
    constexpr double kMaximumExposureMs = 1000.0;

    // One camera. It stands in for the handle a vendor SDK gives out for a camera.
    class ExampleCamera
    {
    public:
        ExampleCamera(QString id, std::shared_ptr<const TestPatterns> patterns, int sensorWidth, int sensorHeight)
            : m_id(std::move(id))
              , m_patterns(std::move(patterns))
              , m_sensorWidth(sensorWidth)
              , m_sensorHeight(sensorHeight)
              , m_roiWidth(sensorWidth)
              , m_roiHeight(sensorHeight)
        {
        }

        ~ExampleCamera()
        {
            stop();
        }

        const QString& id() const { return m_id; }

        void setFrameSink(Camera::FrameSink sink)
        {
            QMutexLocker locker(&m_mutex);
            m_sink = std::move(sink);
        }

        bool start()
        {
            if (m_running.exchange(true))
            {
                return true;
            }
            m_thread = std::thread([this]()
            {
                while (m_running.load())
                {
                    const ImageFrame frame = expose();
                    Camera::FrameSink sink;
                    {
                        QMutexLocker locker(&m_mutex);
                        sink = m_sink;
                    }
                    if (sink && m_running.load())
                    {
                        sink(frame);
                    }
                }
            });
            return true;
        }

        bool stop()
        {
            m_running.store(false);
            if (m_thread.joinable())
            {
                m_thread.join();
            }
            return true;
        }

        bool isRunning() const { return m_running.load(); }

        double exposureMs() const
        {
            QMutexLocker locker(&m_mutex);
            return m_exposureMs;
        }

        bool setExposureMs(double exposureMs)
        {
            if (exposureMs < kMinimumExposureMs || exposureMs > kMaximumExposureMs)
            {
                return false;
            }
            QMutexLocker locker(&m_mutex);
            m_exposureMs = exposureMs;
            return true;
        }

        bool setRoi(int x, int y, int width, int height)
        {
            if (x < 0 || y < 0 || width <= 0 || height <= 0
                || x + width > m_sensorWidth || y + height > m_sensorHeight)
            {
                return false;
            }
            QMutexLocker locker(&m_mutex);
            m_roiX = x;
            m_roiY = y;
            m_roiWidth = width;
            m_roiHeight = height;
            return true;
        }

        void roi(int& x, int& y, int& width, int& height) const
        {
            QMutexLocker locker(&m_mutex);
            x = m_roiX;
            y = m_roiY;
            width = m_roiWidth;
            height = m_roiHeight;
        }

        bool clearRoi()
        {
            return setRoi(0, 0, m_sensorWidth, m_sensorHeight);
        }

        int gain() const
        {
            QMutexLocker locker(&m_mutex);
            return m_gain;
        }

        bool setGain(int gain)
        {
            if (gain < 1 || gain > 16)
            {
                return false;
            }
            QMutexLocker locker(&m_mutex);
            m_gain = gain;
            return true;
        }

        TestPattern pattern() const
        {
            QMutexLocker locker(&m_mutex);
            return m_pattern;
        }

        void setPattern(TestPattern pattern)
        {
            QMutexLocker locker(&m_mutex);
            m_pattern = pattern;
        }

        // Waits one exposure and reads out the region of interest
        ImageFrame expose()
        {
            double exposureMs = 0.0;
            int roiX = 0;
            int roiY = 0;
            int roiWidth = 0;
            int roiHeight = 0;
            int gain = 1;
            TestPattern pattern = TestPattern::MovingSpot;
            {
                QMutexLocker locker(&m_mutex);
                pattern = m_pattern;
                exposureMs = m_exposureMs;
                roiX = m_roiX;
                roiY = m_roiY;
                roiWidth = m_roiWidth;
                roiHeight = m_roiHeight;
                gain = m_gain;
            }
            std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(exposureMs));

            // A single capture checked only that the acquisition was not running, so a preview started
            // meanwhile read out on its thread at the same time and both wrote the buffers below
            QMutexLocker readout(&m_readoutMutex);
            ImageFrame frame;
            frame.cameraId = m_id;
            frame.width = roiWidth;
            frame.height = roiHeight;
            frame.stride = roiWidth * 2;
            frame.bitsPerSample = kBitsPerSample;
            frame.pixelFormat = ImagePixelFormat::Mono16;
            frame.frameIndex = m_frameIndex++;
            frame.timestampNs = static_cast<quint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            frame.sourceRoiX = roiX;
            frame.sourceRoiY = roiY;
            frame.sourceRoiWidth = roiWidth;
            frame.sourceRoiHeight = roiHeight;
            frame.bytes.resize(static_cast<qsizetype>(frame.stride) * roiHeight);

            // The signal grows with the exposure and the gain, 16 counts of the 12-bit sensor for each level of
            // the pattern at the default exposure, and the shot noise of the sensor is drawn around it
            m_patterns->render(pattern, frame.frameIndex, roiX, roiY, roiWidth, roiHeight, m_intensities);
            const double scale = 16.0 * gain * exposureMs / 10.0;
            const double maximum = static_cast<double>((1 << kBitsPerSample) - 1);
            auto* pixels = reinterpret_cast<quint16*>(frame.bytes.data());
            for (std::size_t index = 0; index < m_intensities.size(); ++index)
            {
                const double counts = scale * m_intensities[index];
                std::normal_distribution<double> noise(counts, std::sqrt(std::max(counts, 1.0)));
                pixels[index] = static_cast<quint16>(std::clamp(noise(m_random), 0.0, maximum));
            }
            return frame;
        }

    private:
        const QString m_id;
        const std::shared_ptr<const TestPatterns> m_patterns;
        const int m_sensorWidth;
        const int m_sensorHeight;
        mutable QMutex m_mutex;
        QMutex m_readoutMutex;
        Camera::FrameSink m_sink;
        double m_exposureMs{10.0};
        int m_gain{1};
        TestPattern m_pattern{TestPattern::MovingSpot};
        int m_roiX{0};
        int m_roiY{0};
        int m_roiWidth;
        int m_roiHeight;
        quint64 m_frameIndex{0};
        std::vector<double> m_intensities;
        std::minstd_rand m_random{12345};
        std::atomic_bool m_running{false};
        std::thread m_thread;
    };

    // One camera per device, and a second camera is a second device line of the cfg. A device
    // without peripherals is addressed with an empty device id, so the ids of the calls are not
    // looked at.
    class ExampleCameraDevice final : public Device, public Camera
    {
    public:
        ExampleCameraDevice(const QString& driverId, int width, int height)
            : m_driverId(driverId)
              , m_camera(QString{}, std::make_shared<const TestPatterns>(width, height), width, height)
        {
        }

        HardwareProviderDescriptor descriptor() const override
        {
            return {m_driverId, QStringLiteral("Example Camera"), QStringLiteral("1")};
        }

        void setFrameSink(FrameSink sink) override
        {
            m_camera.setFrameSink(std::move(sink));
        }

        void setPreviewStateSink(PreviewStateSink sink) override
        {
            QMutexLocker locker(&m_mutex);
            m_previewStateSink = std::move(sink);
        }

        bool startPreview(const QString&) override
        {
            m_camera.start();
            reportPreviewState();
            return true;
        }

        bool stopPreview(const QString&) override
        {
            m_camera.stop();
            reportPreviewState();
            return true;
        }

        bool isPreviewRunning(const QString&) const override
        {
            return m_camera.isRunning();
        }

        bool getExposure(const QString&, double& exposureMs) const override
        {
            exposureMs = m_camera.exposureMs();
            return true;
        }

        bool setExposure(const QString&, double exposureMs) override
        {
            return m_camera.setExposureMs(exposureMs);
        }

        bool setROI(const QString&, int x, int y, int width, int height) override
        {
            return m_camera.setRoi(x, y, width, height);
        }

        bool clearROI(const QString&) override
        {
            return m_camera.clearRoi();
        }

        bool getROI(const QString&, int& x, int& y, int& width, int& height) override
        {
            m_camera.roi(x, y, width, height);
            return true;
        }

        // A software trigger: one exposure, read out while the caller waits
        bool snap(const QString&, ImageFrame& frame, int timeoutMs) override
        {
            if (m_camera.isRunning() || m_camera.exposureMs() > timeoutMs)
            {
                return false;
            }
            frame = m_camera.expose();
            return true;
        }

        // The camera has a gain and a test pattern the user can set, and a sensor temperature it reports
        QStringList listProperties(const QString&) override
        {
            return {QStringLiteral("Gain"), QStringLiteral("Pattern"), QStringLiteral("SensorTemperature")};
        }

        QString getProperty(const QString&, const QString& name, bool) override
        {
            if (name == QStringLiteral("Gain"))
            {
                return QString::number(m_camera.gain());
            }
            if (name == QStringLiteral("Pattern"))
            {
                return testPatternName(m_camera.pattern());
            }
            if (name == QStringLiteral("SensorTemperature"))
            {
                return QStringLiteral("-20.0");
            }
            return {};
        }

        bool setProperty(const QString&,
                         const QString& name,
                         const QString& value,
                         QString* errorMessage) override
        {
            TestPattern pattern = TestPattern::MovingSpot;
            if (name == QStringLiteral("Pattern") && testPatternFromName(value, pattern))
            {
                m_camera.setPattern(pattern);
                return true;
            }
            bool numeric = false;
            const int gain = value.toInt(&numeric);
            if (name == QStringLiteral("Gain") && numeric && m_camera.setGain(gain))
            {
                return true;
            }
            if (errorMessage)
            {
                *errorMessage = name == QStringLiteral("Gain") ? QStringLiteral("Gain must be an integer from 1 to 16")
                              : name == QStringLiteral("Pattern") ? QStringLiteral("Unknown pattern %1").arg(value)
                                                                  : QStringLiteral("%1 cannot be set").arg(name);
            }
            return false;
        }

        QString getPropertyType(const QString&, const QString& name) override
        {
            return name == QStringLiteral("Gain") ? QStringLiteral("Integer")
                 : name == QStringLiteral("Pattern") ? QStringLiteral("String")
                                                     : QStringLiteral("Float");
        }

        bool isPropertyReadOnly(const QString&, const QString& name) override
        {
            return name == QStringLiteral("SensorTemperature");
        }

        bool isPropertyPreInit(const QString&, const QString&) override
        {
            return false;
        }

        QStringList getAllowedPropertyValues(const QString&, const QString& name) override
        {
            return name == QStringLiteral("Pattern") ? testPatternNames() : QStringList{};
        }

        bool hasPropertyLimits(const QString&, const QString& name) override
        {
            return name == QStringLiteral("Gain");
        }

        double getPropertyLowerLimit(const QString&, const QString& name) override
        {
            return name == QStringLiteral("Gain") ? 1.0 : 0.0;
        }

        double getPropertyUpperLimit(const QString&, const QString& name) override
        {
            return name == QStringLiteral("Gain") ? 16.0 : 0.0;
        }

    private:
        void reportPreviewState()
        {
            const bool running = m_camera.isRunning();
            PreviewStateSink sink;
            {
                QMutexLocker locker(&m_mutex);
                if (running == m_lastPreviewState)
                {
                    return;
                }
                m_lastPreviewState = running;
                sink = m_previewStateSink;
            }
            if (sink)
            {
                sink(running);
            }
        }

        QString m_driverId;
        ExampleCamera m_camera;
        QMutex m_mutex;
        PreviewStateSink m_previewStateSink;
        bool m_lastPreviewState{false};
    };

    class ExampleCameraDriver final : public QObject,
                                        public scopeone::core::Driver
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneDriver_iid FILE "scopeone_driver.json")
        Q_INTERFACES(scopeone::core::Driver)

    public:
        QString driverId() const override
        {
            return QStringLiteral("example.camera");
        }

        QStringList deviceNames() const override
        {
            return {QStringLiteral("Example Camera")};
        }

        // Pre-init properties: width and height of the sensor
        scopeone::core::DevicePtr createDevice(const QString& name,
                                               const QVariantMap& preInitProperties,
                                               QString* errorMessage) override
        {
            if (errorMessage)
            {
                errorMessage->clear();
            }
            const int width = preInitProperties.value(QStringLiteral("width"), 512).toInt();
            const int height = preInitProperties.value(QStringLiteral("height"), 512).toInt();
            if (name != deviceNames().first() || width <= 0 || height <= 0)
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("width and height must be positive");
                }
                return {};
            }
            return std::make_shared<ExampleCameraDevice>(driverId(), width, height);
        }
    };
}

#include "ExampleCameraDriver.moc"

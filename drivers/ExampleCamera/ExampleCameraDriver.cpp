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
    using scopeone::core::HardwareDeviceDescriptor;
    using scopeone::core::HardwareDeviceKind;
    using scopeone::core::HardwareDeviceState;
    using scopeone::core::Device;
    using scopeone::core::Hub;
    using scopeone::core::HardwareProviderDescriptor;
    using scopeone::core::ImageFrame;
    using scopeone::core::ImagePixelFormat;

    constexpr int kBitsPerSample = 12;
    constexpr double kMinimumExposureMs = 1.0;
    constexpr double kMaximumExposureMs = 1000.0;

    // One camera. It stands in for the handle a vendor SDK gives out for each camera it finds.
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

    // One device that offers several cameras, as the driver of a camera maker does when its SDK
    // finds several. It is a hub, and ScopeOne addresses each camera by its id.
    class ExampleCameraDevice final : public Device, public Hub, public Camera
    {
    public:
        ExampleCameraDevice(const QString& providerId, int cameraCount, int width, int height)
            : m_providerId(providerId)
        {
            const auto patterns = std::make_shared<const TestPatterns>(width, height);
            for (int index = 0; index < cameraCount; ++index)
            {
                const QString id = cameraCount == 1
                                       ? QStringLiteral("camera.example")
                                       : QStringLiteral("camera.example.%1").arg(index + 1);
                m_cameras.push_back(std::make_unique<ExampleCamera>(id, patterns, width, height));
            }
        }

        HardwareProviderDescriptor descriptor() const override
        {
            return {m_providerId, QStringLiteral("Example Camera"), QStringLiteral("1")};
        }

        QList<HardwareDeviceDescriptor> peripherals() const override
        {
            QList<HardwareDeviceDescriptor> result;
            for (size_t index = 0; index < m_cameras.size(); ++index)
            {
                HardwareDeviceDescriptor device;
                device.logicalId = m_cameras[index]->id();
                device.providerId = m_providerId;
                device.providerDeviceId = m_cameras[index]->id();
                device.hardwareId = QStringLiteral("EX-%1").arg(1000 + index);
                device.name = QStringLiteral("Example Camera %1").arg(index + 1);
                device.kind = HardwareDeviceKind::Camera;
                device.state = HardwareDeviceState::Initialized;
                result.append(device);
            }
            return result;
        }

        // The frames carry the id of the camera that made them, so one sink serves all of them
        void setFrameSink(FrameSink sink) override
        {
            for (auto& camera : m_cameras)
            {
                camera->setFrameSink(sink);
            }
        }

        void setPreviewStateSink(PreviewStateSink sink) override
        {
            QMutexLocker locker(&m_mutex);
            m_previewStateSink = std::move(sink);
        }

        bool startPreviewFor(const QString& cameraId) override
        {
            ExampleCamera* camera = find(cameraId);
            if (!camera)
            {
                return false;
            }
            camera->start();
            reportPreviewState();
            return true;
        }

        bool stopPreviewFor(const QString& cameraId) override
        {
            ExampleCamera* camera = find(cameraId);
            if (!camera)
            {
                return false;
            }
            camera->stop();
            reportPreviewState();
            return true;
        }

        bool isPreviewRunning(const QString& cameraId) const override
        {
            const ExampleCamera* camera = find(cameraId);
            return camera && camera->isRunning();
        }

        bool getExposure(const QString& cameraId, double& exposureMs) const override
        {
            const ExampleCamera* camera = find(cameraId);
            if (!camera)
            {
                return false;
            }
            exposureMs = camera->exposureMs();
            return true;
        }

        bool setExposure(const QString& cameraId, double exposureMs) override
        {
            ExampleCamera* camera = find(cameraId);
            return camera && camera->setExposureMs(exposureMs);
        }

        bool setROI(const QString& cameraId, int x, int y, int width, int height) override
        {
            ExampleCamera* camera = find(cameraId);
            return camera && camera->setRoi(x, y, width, height);
        }

        bool clearROI(const QString& cameraId) override
        {
            ExampleCamera* camera = find(cameraId);
            return camera && camera->clearRoi();
        }

        bool getROI(const QString& cameraId, int& x, int& y, int& width, int& height) override
        {
            ExampleCamera* camera = find(cameraId);
            if (!camera)
            {
                return false;
            }
            camera->roi(x, y, width, height);
            return true;
        }

        // A software trigger: one exposure, read out while the caller waits
        bool captureEventFrame(const QString& cameraId, ImageFrame& frame, int timeoutMs) override
        {
            ExampleCamera* camera = find(cameraId);
            if (!camera || camera->isRunning() || camera->exposureMs() > timeoutMs)
            {
                return false;
            }
            frame = camera->expose();
            return true;
        }

        // Each camera has a gain and a test pattern the user can set, and a sensor temperature it reports
        QStringList listProperties(const QString& deviceId) override
        {
            return find(deviceId) ? QStringList{QStringLiteral("Gain"), QStringLiteral("Pattern"),
                                                QStringLiteral("SensorTemperature")}
                                  : QStringList{};
        }

        QString getProperty(const QString& deviceId, const QString& name, bool) override
        {
            ExampleCamera* camera = find(deviceId);
            if (!camera)
            {
                return {};
            }
            if (name == QStringLiteral("Gain"))
            {
                return QString::number(camera->gain());
            }
            if (name == QStringLiteral("Pattern"))
            {
                return testPatternName(camera->pattern());
            }
            if (name == QStringLiteral("SensorTemperature"))
            {
                return QStringLiteral("-20.0");
            }
            return {};
        }

        bool setProperty(const QString& deviceId,
                         const QString& name,
                         const QString& value,
                         QString* errorMessage) override
        {
            ExampleCamera* camera = find(deviceId);
            TestPattern pattern = TestPattern::MovingSpot;
            if (camera && name == QStringLiteral("Pattern") && testPatternFromName(value, pattern))
            {
                camera->setPattern(pattern);
                return true;
            }
            bool numeric = false;
            const int gain = value.toInt(&numeric);
            if (camera && name == QStringLiteral("Gain") && numeric && camera->setGain(gain))
            {
                return true;
            }
            if (errorMessage)
            {
                *errorMessage = !camera ? QStringLiteral("Unknown camera")
                              : name == QStringLiteral("Gain") ? QStringLiteral("Gain must be an integer from 1 to 16")
                              : name == QStringLiteral("Pattern") ? QStringLiteral("Unknown pattern %1").arg(value)
                                                                  : QStringLiteral("%1 cannot be set").arg(name);
            }
            return false;
        }

        QString getPropertyType(const QString& deviceId, const QString& name) override
        {
            if (!find(deviceId))
            {
                return {};
            }
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
        ExampleCamera* find(const QString& cameraId) const
        {
            for (const auto& camera : m_cameras)
            {
                if (camera->id() == cameraId)
                {
                    return camera.get();
                }
            }
            return nullptr;
        }

        // The provider is previewing while any of its cameras is
        void reportPreviewState()
        {
            const bool running = std::any_of(m_cameras.begin(), m_cameras.end(),
                                              [](const auto& camera) { return camera->isRunning(); });
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

        QString m_providerId;
        std::vector<std::unique_ptr<ExampleCamera>> m_cameras;
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
        QString providerId() const override
        {
            return QStringLiteral("example.camera");
        }

        QStringList deviceNames() const override
        {
            return {QStringLiteral("Example Camera")};
        }

        // Pre-init properties: width and height of the sensor, and cameraCount, the number of
        // cameras (2 by default)
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
            const int cameraCount = preInitProperties.value(QStringLiteral("cameraCount"), 2).toInt();
            if (name != deviceNames().first()
                || width <= 0 || height <= 0 || cameraCount < 1 || cameraCount > 8)
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("width and height must be positive and cameraCount from 1 to 8");
                }
                return {};
            }
            return std::make_shared<ExampleCameraDevice>(providerId(), cameraCount, width, height);
        }
    };
}

#include "ExampleCameraDriver.moc"

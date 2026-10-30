#include "scopeone/CameraProvider.h"
#include "scopeone/DriverHostProviderPlugin.h"
#include "scopeone/HardwareProvider.h"
#include "scopeone/SimulatorProvider.h"

#include <QMutex>
#include <QMutexLocker>
#include <QObject>

#include <memory>
#include <vector>

namespace
{
    using scopeone::core::CameraProvider;
    using scopeone::core::HardwareDeviceDescriptor;
    using scopeone::core::HardwareProvider;
    using scopeone::core::HardwareProviderDescriptor;
    using scopeone::core::ImageFrame;
    using scopeone::core::SimulatorProvider;

    // One provider that offers several cameras, as the driver of a real camera maker does when it finds
    // more than one camera. Every camera has its own id, exposure, region of interest and preview, and
    // the provider tells them apart by that id. Each camera here is a simulated one.
    class MultiCameraProvider final : public HardwareProvider, public CameraProvider
    {
    public:
        MultiCameraProvider(const QString& providerId, int cameraCount, int width, int height)
            : m_providerId(providerId)
        {
            for (int index = 0; index < cameraCount; ++index)
            {
                // A single camera keeps the plain id that this example always had
                const QString id = cameraCount == 1
                                       ? QStringLiteral("camera.example")
                                       : QStringLiteral("camera.example.%1").arg(index + 1);
                m_ids.append(id);
                m_cameras.push_back(std::make_unique<SimulatorProvider>(id, width, height, providerId));
            }
        }

        HardwareProviderDescriptor descriptor() const override
        {
            return {m_providerId, QStringLiteral("Example Hardware"), QStringLiteral("1")};
        }

        QList<HardwareDeviceDescriptor> devices() const override
        {
            QList<HardwareDeviceDescriptor> result;
            for (size_t index = 0; index < m_cameras.size(); ++index)
            {
                for (HardwareDeviceDescriptor device : m_cameras[index]->devices())
                {
                    device.name = QStringLiteral("Example Camera %1").arg(index + 1);
                    result.append(device);
                }
            }
            return result;
        }

        void setFrameSink(FrameSink sink) override
        {
            // The frames carry the id of the camera that made them, so one sink serves all of them
            for (auto& camera : m_cameras)
            {
                camera->setFrameSink(sink);
            }
        }

        // The provider is previewing while any of its cameras is
        void setPreviewStateSink(PreviewStateSink sink) override
        {
            {
                QMutexLocker locker(&m_mutex);
                m_previewStateSink = std::move(sink);
            }
            for (auto& camera : m_cameras)
            {
                camera->setPreviewStateSink([this](bool) { reportPreviewState(); });
            }
        }

        bool startPreview() override
        {
            bool all = true;
            for (auto& camera : m_cameras)
            {
                all = camera->startPreview() && all;
            }
            return all;
        }

        bool stopPreview() override
        {
            bool all = true;
            for (auto& camera : m_cameras)
            {
                all = camera->stopPreview() && all;
            }
            return all;
        }

        bool startPreviewFor(const QString& cameraId) override
        {
            SimulatorProvider* camera = owner(cameraId);
            return camera && camera->startPreviewFor(cameraId);
        }

        bool stopPreviewFor(const QString& cameraId) override
        {
            SimulatorProvider* camera = owner(cameraId);
            return camera && camera->stopPreviewFor(cameraId);
        }

        bool isPreviewRunning(const QString& cameraId) const override
        {
            const SimulatorProvider* camera = owner(cameraId);
            return camera && camera->isPreviewRunning(cameraId);
        }

        bool getExposure(const QString& cameraIdOrAll, double& exposureMs) const override
        {
            if (const SimulatorProvider* camera = owner(cameraIdOrAll))
            {
                return camera->getExposure(cameraIdOrAll, exposureMs);
            }
            return !m_cameras.empty() && m_cameras.front()->getExposure(m_ids.first(), exposureMs);
        }

        // "All" sets the exposure of every camera
        bool setExposure(const QString& cameraIdOrAll, double exposureMs) override
        {
            if (SimulatorProvider* camera = owner(cameraIdOrAll))
            {
                return camera->setExposure(cameraIdOrAll, exposureMs);
            }
            bool any = false;
            for (size_t index = 0; index < m_cameras.size(); ++index)
            {
                any = m_cameras[index]->setExposure(m_ids.at(static_cast<qsizetype>(index)), exposureMs) || any;
            }
            return any;
        }

        bool setROI(const QString& cameraId, int x, int y, int width, int height) override
        {
            SimulatorProvider* camera = owner(cameraId);
            return camera && camera->setROI(cameraId, x, y, width, height);
        }

        bool clearROI(const QString& cameraId) override
        {
            SimulatorProvider* camera = owner(cameraId);
            return camera && camera->clearROI(cameraId);
        }

        bool getROI(const QString& cameraId, int& x, int& y, int& width, int& height) override
        {
            SimulatorProvider* camera = owner(cameraId);
            return camera && camera->getROI(cameraId, x, y, width, height);
        }

        bool captureEventFrame(const QString& cameraId, ImageFrame& frame, int timeoutMs) override
        {
            SimulatorProvider* camera = owner(cameraId);
            return camera && camera->captureEventFrame(cameraId, frame, timeoutMs);
        }

        QStringList listProperties(const QString& deviceId) override
        {
            SimulatorProvider* camera = owner(deviceId);
            return camera ? camera->listProperties(deviceId) : QStringList{};
        }

        QString getProperty(const QString& deviceId, const QString& name, bool fromCache) override
        {
            SimulatorProvider* camera = owner(deviceId);
            return camera ? camera->getProperty(deviceId, name, fromCache) : QString{};
        }

        bool setProperty(const QString& deviceId,
                         const QString& name,
                         const QString& value,
                         QString* errorMessage) override
        {
            SimulatorProvider* camera = owner(deviceId);
            if (!camera)
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Unknown camera");
                }
                return false;
            }
            return camera->setProperty(deviceId, name, value, errorMessage);
        }

        QString getPropertyType(const QString& deviceId, const QString& name) override
        {
            SimulatorProvider* camera = owner(deviceId);
            return camera ? camera->getPropertyType(deviceId, name) : QString{};
        }

        bool isPropertyReadOnly(const QString& deviceId, const QString& name) override
        {
            SimulatorProvider* camera = owner(deviceId);
            return !camera || camera->isPropertyReadOnly(deviceId, name);
        }

        bool isPropertyPreInit(const QString& deviceId, const QString& name) override
        {
            SimulatorProvider* camera = owner(deviceId);
            return camera && camera->isPropertyPreInit(deviceId, name);
        }

        QStringList getAllowedPropertyValues(const QString& deviceId, const QString& name) override
        {
            SimulatorProvider* camera = owner(deviceId);
            return camera ? camera->getAllowedPropertyValues(deviceId, name) : QStringList{};
        }

        bool hasPropertyLimits(const QString& deviceId, const QString& name) override
        {
            SimulatorProvider* camera = owner(deviceId);
            return camera && camera->hasPropertyLimits(deviceId, name);
        }

        double getPropertyLowerLimit(const QString& deviceId, const QString& name) override
        {
            SimulatorProvider* camera = owner(deviceId);
            return camera ? camera->getPropertyLowerLimit(deviceId, name) : 0.0;
        }

        double getPropertyUpperLimit(const QString& deviceId, const QString& name) override
        {
            SimulatorProvider* camera = owner(deviceId);
            return camera ? camera->getPropertyUpperLimit(deviceId, name) : 0.0;
        }

    private:
        // The camera with this id, or none for another id such as "all"
        SimulatorProvider* owner(const QString& cameraId) const
        {
            const qsizetype index = m_ids.indexOf(cameraId);
            return index >= 0 ? m_cameras[static_cast<size_t>(index)].get() : nullptr;
        }

        void reportPreviewState()
        {
            bool running = false;
            for (size_t index = 0; index < m_cameras.size(); ++index)
            {
                running = running || m_cameras[index]->isPreviewRunning(m_ids.at(static_cast<qsizetype>(index)));
            }
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
        QStringList m_ids;
        std::vector<std::unique_ptr<SimulatorProvider>> m_cameras;
        QMutex m_mutex;
        PreviewStateSink m_previewStateSink;
        bool m_lastPreviewState{false};
    };

    class ExampleHardwarePlugin final : public QObject,
                                        public scopeone::core::DriverHostProviderPlugin
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneDriverHostProviderPlugin_iid FILE "scopeone_plugin.json")
        Q_INTERFACES(scopeone::core::DriverHostProviderPlugin)

    public:
        QString providerId() const override
        {
            return QStringLiteral("example.hardware");
        }

        // Options: width and height of the images, and cameraCount, the number of cameras (2 by default)
        scopeone::core::HardwareProviderPtr createProvider(const QJsonObject& options,
                                                            QString* errorMessage) override
        {
            if (errorMessage)
            {
                errorMessage->clear();
            }
            const int width = options.value(QStringLiteral("width")).toInt(512);
            const int height = options.value(QStringLiteral("height")).toInt(512);
            const int cameraCount = options.value(QStringLiteral("cameraCount")).toInt(2);
            if (width <= 0 || height <= 0 || cameraCount < 1 || cameraCount > 8)
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("width and height must be positive and cameraCount from 1 to 8");
                }
                return {};
            }
            return std::make_shared<MultiCameraProvider>(providerId(), cameraCount, width, height);
        }
    };
}

#include "ExampleHardwarePlugin.moc"

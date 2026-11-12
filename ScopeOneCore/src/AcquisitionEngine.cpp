#include "internal/AcquisitionEngine.h"

#include "scopeone/Camera.h"
#include "internal/HardwareRuntime.h"

namespace scopeone::core::internal
{
    AcquisitionEngine::AcquisitionEngine(DeviceRegistry& deviceRegistry)
        : m_deviceRegistry(deviceRegistry)
    {
    }

    bool AcquisitionEngine::start(const QString& cameraIdOrAll)
    {
        const QString target = cameraIdOrAll.trimmed();
        if (target.isEmpty())
        {
            return false;
        }
        if (target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
        {
            struct StartedCamera
            {
                Camera* provider;
                QString cameraId;
            };
            QList<StartedCamera> startedCameras;
            bool found = false;
            for (const HardwareDeviceDescriptor& device : m_deviceRegistry.devices())
            {
                if (device.kind != HardwareDeviceKind::Camera)
                {
                    continue;
                }
                found = true;
                const DevicePtr provider = m_deviceRegistry.provider(device.providerId);
                auto* cameraProvider = dynamic_cast<Camera*>(provider.get());
                if (!cameraProvider)
                {
                    for (auto it = startedCameras.crbegin(); it != startedCameras.crend(); ++it)
                    {
                        it->provider->stopPreview(it->cameraId);
                    }
                    return false;
                }
                if (cameraProvider->isPreviewRunning(device.logicalId))
                {
                    continue;
                }
                if (!cameraProvider->startPreview(device.logicalId))
                {
                    for (auto it = startedCameras.crbegin(); it != startedCameras.crend(); ++it)
                    {
                        it->provider->stopPreview(it->cameraId);
                    }
                    return false;
                }
                startedCameras.append({cameraProvider, device.logicalId});
            }
            return found;
        }
        const DevicePtr provider = m_deviceRegistry.providerForDevice(target);
        auto* cameraProvider = dynamic_cast<Camera*>(provider.get());
        return cameraProvider && cameraProvider->startPreview(target);
    }

    bool AcquisitionEngine::stop(const QString& cameraIdOrAll)
    {
        const QString target = cameraIdOrAll.trimmed();
        if (target.isEmpty())
        {
            return false;
        }
        if (target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
        {
            bool found = false;
            bool stopped = true;
            for (const HardwareDeviceDescriptor& device : m_deviceRegistry.devices())
            {
                if (device.kind != HardwareDeviceKind::Camera)
                {
                    continue;
                }
                found = true;
                const DevicePtr provider = m_deviceRegistry.provider(device.providerId);
                auto* cameraProvider = dynamic_cast<Camera*>(provider.get());
                if (!cameraProvider)
                {
                    stopped = false;
                    continue;
                }
                if (cameraProvider->isPreviewRunning(device.logicalId))
                {
                    stopped = cameraProvider->stopPreview(device.logicalId) && stopped;
                }
            }
            return found && stopped;
        }
        const DevicePtr provider = m_deviceRegistry.providerForDevice(target);
        auto* cameraProvider = dynamic_cast<Camera*>(provider.get());
        return cameraProvider && cameraProvider->stopPreview(target);
    }
}

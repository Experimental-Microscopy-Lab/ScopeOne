#include "internal/AcquisitionEngine.h"

#include "scopeone/CameraProvider.h"
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
                CameraProvider* provider;
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
                const HardwareProviderPtr provider = m_deviceRegistry.provider(device.providerId);
                auto* cameraProvider = dynamic_cast<CameraProvider*>(provider.get());
                if (!cameraProvider)
                {
                    for (auto it = startedCameras.crbegin(); it != startedCameras.crend(); ++it)
                    {
                        it->provider->stopPreviewFor(it->cameraId);
                    }
                    return false;
                }
                if (cameraProvider->isPreviewRunning(device.logicalId))
                {
                    continue;
                }
                if (!cameraProvider->startPreviewFor(device.logicalId))
                {
                    for (auto it = startedCameras.crbegin(); it != startedCameras.crend(); ++it)
                    {
                        it->provider->stopPreviewFor(it->cameraId);
                    }
                    return false;
                }
                startedCameras.append({cameraProvider, device.logicalId});
            }
            return found;
        }
        const HardwareProviderPtr provider = m_deviceRegistry.providerForDevice(target);
        auto* cameraProvider = dynamic_cast<CameraProvider*>(provider.get());
        return cameraProvider && cameraProvider->startPreviewFor(target);
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
                const HardwareProviderPtr provider = m_deviceRegistry.provider(device.providerId);
                auto* cameraProvider = dynamic_cast<CameraProvider*>(provider.get());
                if (!cameraProvider)
                {
                    stopped = false;
                    continue;
                }
                if (cameraProvider->isPreviewRunning(device.logicalId))
                {
                    stopped = cameraProvider->stopPreviewFor(device.logicalId) && stopped;
                }
            }
            return found && stopped;
        }
        const HardwareProviderPtr provider = m_deviceRegistry.providerForDevice(target);
        auto* cameraProvider = dynamic_cast<CameraProvider*>(provider.get());
        return cameraProvider && cameraProvider->stopPreviewFor(target);
    }
}

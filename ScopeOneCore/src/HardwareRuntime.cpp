#include "internal/HardwareRuntime.h"

#include "scopeone/CameraProvider.h"

#include <algorithm>
#include <QSet>
#include <utility>

namespace scopeone::core::internal
{
    DeviceRegistry::DeviceRegistry(QObject* parent)
        : QObject(parent)
    {
    }

    void DeviceRegistry::clear()
    {
        if (m_providers.isEmpty())
        {
            return;
        }
        m_providers.clear();
        emit changed();
    }

    bool DeviceRegistry::registerProvider(const HardwareProviderPtr& provider)
    {
        if (!provider)
        {
            return false;
        }
        const HardwareProviderDescriptor descriptor = provider->descriptor();
        const QString providerId = descriptor.id.trimmed();
        if (providerId.isEmpty())
        {
            return false;
        }
        const QList<HardwareDeviceDescriptor> devices = provider->devices();
        QSet<QString> logicalIds;
        for (const HardwareDeviceDescriptor& device : devices)
        {
            const QString logicalId = device.logicalId.trimmed();
            if (logicalId.isEmpty()
                || device.providerId.trimmed() != providerId
                || logicalIds.contains(logicalId))
            {
                return false;
            }
            const HardwareDeviceDescriptor existing = this->device(logicalId);
            if (!existing.logicalId.isEmpty() && existing.providerId != providerId)
            {
                return false;
            }
            logicalIds.insert(logicalId);
        }
        ProviderEntry entry;
        entry.provider = provider;
        entry.devices = devices;
        m_providers.insert(providerId, std::move(entry));
        emit changed();
        return true;
    }

    void DeviceRegistry::unregisterProvider(const QString& providerId)
    {
        if (m_providers.remove(providerId.trimmed()) > 0)
        {
            emit changed();
        }
    }

    void DeviceRegistry::refreshProvider(const QString& providerId)
    {
        const QString normalizedId = providerId.trimmed();
        const auto it = m_providers.constFind(normalizedId);
        if (it == m_providers.constEnd() || !it->provider)
        {
            return;
        }
        const HardwareProviderPtr provider = it->provider;
        registerProvider(provider);
    }

    QList<HardwareProviderDescriptor> DeviceRegistry::providers() const
    {
        QList<HardwareProviderDescriptor> result;
        result.reserve(m_providers.size());
        for (auto it = m_providers.constBegin(); it != m_providers.constEnd(); ++it)
        {
            result.append(it->provider->descriptor());
        }
        std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs)
        {
            return lhs.id < rhs.id;
        });
        return result;
    }

    QList<HardwareDeviceDescriptor> DeviceRegistry::devices() const
    {
        QList<HardwareDeviceDescriptor> result;
        for (auto it = m_providers.constBegin(); it != m_providers.constEnd(); ++it)
        {
            result.append(it->devices);
        }
        std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs)
        {
            return lhs.logicalId < rhs.logicalId;
        });
        return result;
    }

    HardwareDeviceDescriptor DeviceRegistry::device(const QString& logicalId) const
    {
        const QString normalizedId = logicalId.trimmed();
        for (auto it = m_providers.constBegin(); it != m_providers.constEnd(); ++it)
        {
            for (const HardwareDeviceDescriptor& candidate : it->devices)
            {
                if (candidate.logicalId == normalizedId)
                {
                    return candidate;
                }
            }
        }
        return {};
    }

    HardwareProviderPtr DeviceRegistry::provider(const QString& providerId) const
    {
        const auto it = m_providers.constFind(providerId.trimmed());
        return it == m_providers.constEnd() ? HardwareProviderPtr{} : it->provider;
    }

    HardwareProviderPtr DeviceRegistry::providerForDevice(const QString& logicalId) const
    {
        const HardwareDeviceDescriptor descriptor = device(logicalId);
        return descriptor.logicalId.isEmpty() ? HardwareProviderPtr{} : provider(descriptor.providerId);
    }

    HardwareRuntime::HardwareRuntime(QObject* parent)
        : QObject(parent)
          , m_registry(this)
          , m_frameRouter(&m_clockService, this)
          , m_acquisitionEngine(&m_registry, this)
    {
        connect(&m_registry, &DeviceRegistry::changed,
                this, &HardwareRuntime::devicesChanged);
        connect(&m_frameRouter, &FrameRouter::frameReady,
                this, [this](const ImageFrame& frame)
                {
                    if (m_frameSink)
                    {
                        m_frameSink(frame);
                    }
                });
    }

    void HardwareRuntime::setFrameSink(FrameSink sink)
    {
        m_frameSink = std::move(sink);
    }

    CameraProvider* HardwareRuntime::cameraProviderForDevice(const QString& logicalId) const
    {
        const HardwareProviderPtr provider = m_registry.providerForDevice(logicalId);
        return dynamic_cast<CameraProvider*>(provider.get());
    }

    QList<CameraProvider*> HardwareRuntime::cameraProviders() const
    {
        QList<CameraProvider*> result;
        QSet<CameraProvider*> visited;
        for (const HardwareDeviceDescriptor& device : m_registry.devices())
        {
            if (device.kind != HardwareDeviceKind::Camera)
            {
                continue;
            }
            const HardwareProviderPtr provider = m_registry.provider(device.providerId);
            auto* cameraProvider = dynamic_cast<CameraProvider*>(provider.get());
            if (cameraProvider && !visited.contains(cameraProvider))
            {
                visited.insert(cameraProvider);
                result.append(cameraProvider);
            }
        }
        return result;
    }

    bool HardwareRuntime::startPreview()
    {
        return m_acquisitionEngine.start(QStringLiteral("All"));
    }

    bool HardwareRuntime::stopPreview()
    {
        return m_acquisitionEngine.stop(QStringLiteral("All"));
    }

    bool HardwareRuntime::startPreviewFor(const QString& cameraId)
    {
        return m_acquisitionEngine.start(cameraId);
    }

    bool HardwareRuntime::stopPreviewFor(const QString& cameraId)
    {
        return m_acquisitionEngine.stop(cameraId);
    }

    bool HardwareRuntime::isPreviewRunning(const QString& cameraId) const
    {
        CameraProvider* provider = cameraProviderForDevice(cameraId);
        return provider && provider->isPreviewRunning(cameraId);
    }

    bool HardwareRuntime::getExposure(const QString& cameraIdOrAll, double& exposureMs) const
    {
        const QString target = cameraIdOrAll.trimmed();
        if (target.compare(QStringLiteral("All"), Qt::CaseInsensitive) != 0)
        {
            CameraProvider* provider = cameraProviderForDevice(target);
            return provider && provider->getExposure(target, exposureMs);
        }
        const QList<CameraProvider*> providers = cameraProviders();
        return !providers.isEmpty() && providers.first()->getExposure(QStringLiteral("All"), exposureMs);
    }

    bool HardwareRuntime::setExposure(const QString& cameraIdOrAll, double exposureMs)
    {
        const QString target = cameraIdOrAll.trimmed();
        if (target.compare(QStringLiteral("All"), Qt::CaseInsensitive) != 0)
        {
            CameraProvider* provider = cameraProviderForDevice(target);
            return provider && provider->setExposure(target, exposureMs);
        }
        const QList<CameraProvider*> providers = cameraProviders();
        bool ok = !providers.isEmpty();
        for (CameraProvider* provider : providers)
        {
            ok = provider->setExposure(QStringLiteral("All"), exposureMs) && ok;
        }
        return ok;
    }

    QStringList HardwareRuntime::listProperties(const QString& cameraId)
    {
        CameraProvider* provider = cameraProviderForDevice(cameraId);
        return provider ? provider->listProperties(cameraId) : QStringList{};
    }

    QString HardwareRuntime::getProperty(const QString& cameraId,
                                         const QString& name,
                                         bool fromCache)
    {
        CameraProvider* provider = cameraProviderForDevice(cameraId);
        return provider ? provider->getProperty(cameraId, name, fromCache) : QString{};
    }

    bool HardwareRuntime::setProperty(const QString& cameraId,
                                      const QString& name,
                                      const QString& value,
                                      QString* errorMessage)
    {
        CameraProvider* provider = cameraProviderForDevice(cameraId);
        return provider && provider->setProperty(cameraId, name, value, errorMessage);
    }

    QString HardwareRuntime::getPropertyType(const QString& cameraId, const QString& name)
    {
        CameraProvider* provider = cameraProviderForDevice(cameraId);
        return provider ? provider->getPropertyType(cameraId, name) : QStringLiteral("Unknown");
    }

    bool HardwareRuntime::isPropertyReadOnly(const QString& cameraId, const QString& name)
    {
        CameraProvider* provider = cameraProviderForDevice(cameraId);
        return !provider || provider->isPropertyReadOnly(cameraId, name);
    }

    bool HardwareRuntime::isPropertyPreInit(const QString& cameraId, const QString& name)
    {
        CameraProvider* provider = cameraProviderForDevice(cameraId);
        return provider && provider->isPropertyPreInit(cameraId, name);
    }

    QStringList HardwareRuntime::getAllowedPropertyValues(const QString& cameraId,
                                                          const QString& name)
    {
        CameraProvider* provider = cameraProviderForDevice(cameraId);
        return provider ? provider->getAllowedPropertyValues(cameraId, name) : QStringList{};
    }

    bool HardwareRuntime::hasPropertyLimits(const QString& cameraId, const QString& name)
    {
        CameraProvider* provider = cameraProviderForDevice(cameraId);
        return provider && provider->hasPropertyLimits(cameraId, name);
    }

    double HardwareRuntime::getPropertyLowerLimit(const QString& cameraId, const QString& name)
    {
        CameraProvider* provider = cameraProviderForDevice(cameraId);
        return provider ? provider->getPropertyLowerLimit(cameraId, name) : 0.0;
    }

    double HardwareRuntime::getPropertyUpperLimit(const QString& cameraId, const QString& name)
    {
        CameraProvider* provider = cameraProviderForDevice(cameraId);
        return provider ? provider->getPropertyUpperLimit(cameraId, name) : 0.0;
    }

    bool HardwareRuntime::setROI(const QString& cameraId,
                                 int x,
                                 int y,
                                 int width,
                                 int height)
    {
        CameraProvider* provider = cameraProviderForDevice(cameraId);
        return provider && provider->setROI(cameraId, x, y, width, height);
    }

    bool HardwareRuntime::clearROI(const QString& cameraId)
    {
        CameraProvider* provider = cameraProviderForDevice(cameraId);
        return provider && provider->clearROI(cameraId);
    }

    bool HardwareRuntime::getROI(const QString& cameraId,
                                 int& x,
                                 int& y,
                                 int& width,
                                 int& height)
    {
        CameraProvider* provider = cameraProviderForDevice(cameraId);
        return provider && provider->getROI(cameraId, x, y, width, height);
    }

    bool HardwareRuntime::captureEventFrame(const QString& cameraId,
                                            ImageFrame& frame,
                                            int timeoutMs)
    {
        CameraProvider* provider = cameraProviderForDevice(cameraId);
        return provider && provider->captureEventFrame(cameraId, frame, timeoutMs);
    }

    void HardwareRuntime::clear()
    {
        m_acquisitionEngine.reset();
        for (const HardwareProviderDescriptor& descriptor : m_registry.providers())
        {
            const HardwareProviderPtr provider = m_registry.provider(descriptor.id);
            if (auto* cameraProvider = dynamic_cast<CameraProvider*>(provider.get()))
            {
                cameraProvider->stopPreview();
                cameraProvider->setFrameSink({});
            }
        }
        m_registry.clear();
    }

    bool HardwareRuntime::registerProvider(const HardwareProviderPtr& provider)
    {
        if (!provider || provider->descriptor().id.trimmed().isEmpty())
        {
            return false;
        }
        const HardwareProviderPtr previous = m_registry.provider(provider->descriptor().id);
        auto* cameraProvider = dynamic_cast<CameraProvider*>(provider.get());
        if (cameraProvider)
        {
            cameraProvider->setFrameSink([this](const ImageFrame& frame)
            {
                m_frameRouter.publish(frame);
            });
        }
        const QList<HardwareDeviceDescriptor> providerDevices = provider->devices();
        const bool providerHasCamera = std::any_of(
            providerDevices.cbegin(),
            providerDevices.cend(),
            [](const HardwareDeviceDescriptor& device)
            {
                return device.kind == HardwareDeviceKind::Camera;
            });
        if (providerHasCamera)
        {
            m_acquisitionEngine.prepare();
        }
        if (!m_registry.registerProvider(provider))
        {
            if (cameraProvider && previous != provider)
            {
                cameraProvider->setFrameSink({});
            }
            const QList<HardwareDeviceDescriptor> registeredDevices = m_registry.devices();
            const bool hasRegisteredCamera = std::any_of(
                registeredDevices.cbegin(),
                registeredDevices.cend(),
                [](const HardwareDeviceDescriptor& device)
                {
                    return device.kind == HardwareDeviceKind::Camera;
                });
            if (!hasRegisteredCamera)
            {
                m_acquisitionEngine.reset();
            }
            return false;
        }
        if (previous && previous != provider)
        {
            if (auto* previousCameraProvider = dynamic_cast<CameraProvider*>(previous.get()))
            {
                previousCameraProvider->stopPreview();
                previousCameraProvider->setFrameSink({});
            }
        }
        if (!providerHasCamera)
        {
            const QList<HardwareDeviceDescriptor> registeredDevices = m_registry.devices();
            const bool hasRegisteredCamera = std::any_of(
                registeredDevices.cbegin(),
                registeredDevices.cend(),
                [](const HardwareDeviceDescriptor& device)
                {
                    return device.kind == HardwareDeviceKind::Camera;
                });
            if (!hasRegisteredCamera)
            {
                m_acquisitionEngine.reset();
            }
        }
        return true;
    }

    void HardwareRuntime::unregisterProvider(const QString& providerId)
    {
        const HardwareProviderPtr provider = m_registry.provider(providerId);
        if (auto* cameraProvider = dynamic_cast<CameraProvider*>(provider.get()))
        {
            cameraProvider->stopPreview();
            cameraProvider->setFrameSink({});
        }
        m_registry.unregisterProvider(providerId);
        const QList<HardwareDeviceDescriptor> devices = m_registry.devices();
        const bool hasCamera = std::any_of(
            devices.cbegin(),
            devices.cend(),
            [](const HardwareDeviceDescriptor& device)
            {
                return device.kind == HardwareDeviceKind::Camera;
            });
        if (!hasCamera)
        {
            m_acquisitionEngine.reset();
        }
    }

    void HardwareRuntime::refreshProvider(const QString& providerId)
    {
        m_registry.refreshProvider(providerId);
        const QList<HardwareDeviceDescriptor> devices = m_registry.devices();
        const bool hasCamera = std::any_of(
            devices.cbegin(),
            devices.cend(),
            [](const HardwareDeviceDescriptor& device)
            {
                return device.kind == HardwareDeviceKind::Camera;
            });
        if (hasCamera)
        {
            m_acquisitionEngine.prepare();
        }
        else
        {
            m_acquisitionEngine.reset();
        }
    }
}

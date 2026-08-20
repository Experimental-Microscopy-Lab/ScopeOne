#include "internal/HardwareRuntime.h"

#include "scopeone/CameraProvider.h"

#include <algorithm>
#include <QMetaObject>
#include <QSet>
#include <QThread>
#include <utility>

namespace scopeone::core::internal
{
    DeviceRegistry::DeviceRegistry(QObject* parent)
        : QObject(parent)
    {
    }

    void DeviceRegistry::clear()
    {
        {
            QWriteLocker locker(&m_lock);
            if (m_providers.isEmpty())
            {
                return;
            }
            m_providers.clear();
        }
        emit changed();
    }

    bool DeviceRegistry::registerProvider(
        const HardwareProviderPtr& provider,
        const HardwareProviderDescriptor& descriptor,
        const QList<HardwareDeviceDescriptor>& devices)
    {
        if (!provider)
        {
            return false;
        }
        const QString providerId = descriptor.id.trimmed();
        if (providerId.isEmpty() || descriptor.id != providerId)
        {
            return false;
        }
        QSet<QString> logicalIds;
        for (const HardwareDeviceDescriptor& device : devices)
        {
            const QString logicalId = device.logicalId.trimmed();
            if (logicalId.isEmpty()
                || device.logicalId != logicalId
                || device.providerId != providerId
                || logicalIds.contains(logicalId))
            {
                return false;
            }
            logicalIds.insert(logicalId);
        }
        ProviderEntry entry;
        entry.provider = provider;
        entry.descriptor = descriptor;
        entry.devices = devices;
        {
            QWriteLocker locker(&m_lock);
            for (auto providerIt = m_providers.cbegin();
                 providerIt != m_providers.cend();
                 ++providerIt)
            {
                if (providerIt.key() == providerId)
                {
                    continue;
                }
                for (const HardwareDeviceDescriptor& existing : providerIt->devices)
                {
                    if (logicalIds.contains(existing.logicalId))
                    {
                        return false;
                    }
                }
            }
            m_providers.insert(providerId, std::move(entry));
        }
        emit changed();
        return true;
    }

    void DeviceRegistry::unregisterProvider(const QString& providerId)
    {
        bool removed = false;
        {
            QWriteLocker locker(&m_lock);
            removed = m_providers.remove(providerId.trimmed());
        }
        if (removed)
        {
            emit changed();
        }
    }

    QList<HardwareProviderDescriptor> DeviceRegistry::providers() const
    {
        QReadLocker locker(&m_lock);
        QList<HardwareProviderDescriptor> result;
        result.reserve(m_providers.size());
        for (auto it = m_providers.constBegin(); it != m_providers.constEnd(); ++it)
        {
            result.append(it->descriptor);
        }
        std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs)
        {
            return lhs.id < rhs.id;
        });
        return result;
    }

    QList<HardwareDeviceDescriptor> DeviceRegistry::devices() const
    {
        QReadLocker locker(&m_lock);
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
        QReadLocker locker(&m_lock);
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
        QReadLocker locker(&m_lock);
        const auto it = m_providers.constFind(providerId.trimmed());
        return it == m_providers.constEnd() ? HardwareProviderPtr{} : it->provider;
    }

    HardwareProviderPtr DeviceRegistry::providerForDevice(const QString& logicalId) const
    {
        const QString normalizedId = logicalId.trimmed();
        QReadLocker locker(&m_lock);
        for (auto providerIt = m_providers.constBegin();
             providerIt != m_providers.constEnd();
             ++providerIt)
        {
            for (const HardwareDeviceDescriptor& device : providerIt->devices)
            {
                if (device.logicalId == normalizedId)
                {
                    return providerIt->provider;
                }
            }
        }
        return {};
    }

    HardwareRuntime::HardwareRuntime(QObject* parent)
        : QObject(parent)
          , m_registry(this)
          , m_frameRouter(this)
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

    void HardwareRuntime::setPreviewStateSink(PreviewStateSink sink)
    {
        m_previewStateSink = std::move(sink);
    }

    CameraProvider* HardwareRuntime::cameraProviderForDevice(const QString& logicalId) const
    {
        const HardwareProviderPtr provider = m_registry.providerForDevice(logicalId);
        return dynamic_cast<CameraProvider*>(provider.get());
    }

    DevicePropertyProvider* HardwareRuntime::propertyProviderForDevice(
        const QString& logicalId) const
    {
        const HardwareProviderPtr provider = m_registry.providerForDevice(logicalId);
        return dynamic_cast<DevicePropertyProvider*>(provider.get());
    }

    StageProvider* HardwareRuntime::stageProviderForDevice(const QString& logicalId) const
    {
        const HardwareProviderPtr provider = m_registry.providerForDevice(logicalId);
        return dynamic_cast<StageProvider*>(provider.get());
    }

    ShutterProvider* HardwareRuntime::shutterProviderForDevice(const QString& logicalId) const
    {
        const HardwareProviderPtr provider = m_registry.providerForDevice(logicalId);
        return dynamic_cast<ShutterProvider*>(provider.get());
    }

    StateProvider* HardwareRuntime::stateProviderForDevice(const QString& logicalId) const
    {
        const HardwareProviderPtr provider = m_registry.providerForDevice(logicalId);
        return dynamic_cast<StateProvider*>(provider.get());
    }

    ConfigurationProvider* HardwareRuntime::configurationProviderForGroup(
        const QString& groupName) const
    {
        const QString normalizedGroup = groupName.trimmed();
        if (normalizedGroup.isEmpty())
        {
            return nullptr;
        }
        ConfigurationProvider* match = nullptr;
        for (const HardwareProviderDescriptor& descriptor : m_registry.providers())
        {
            const HardwareProviderPtr provider = m_registry.provider(descriptor.id);
            auto* configurationProvider = dynamic_cast<ConfigurationProvider*>(provider.get());
            if (!configurationProvider
                || !configurationProvider->availableConfigGroups().contains(normalizedGroup))
            {
                continue;
            }
            if (match)
            {
                return nullptr;
            }
            match = configurationProvider;
        }
        return match;
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
        bool found = false;
        double commonExposureMs = 0.0;
        for (const HardwareDeviceDescriptor& device : m_registry.devices())
        {
            if (device.kind != HardwareDeviceKind::Camera)
            {
                continue;
            }
            CameraProvider* provider = cameraProviderForDevice(device.logicalId);
            double deviceExposureMs = 0.0;
            if (!provider || !provider->getExposure(device.logicalId, deviceExposureMs))
            {
                return false;
            }
            if (found && !qFuzzyCompare(commonExposureMs + 1.0, deviceExposureMs + 1.0))
            {
                return false;
            }
            commonExposureMs = deviceExposureMs;
            found = true;
        }
        if (found)
        {
            exposureMs = commonExposureMs;
        }
        return found;
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
        DevicePropertyProvider* provider = propertyProviderForDevice(cameraId);
        return provider ? provider->listProperties(cameraId) : QStringList{};
    }

    QString HardwareRuntime::getProperty(const QString& cameraId,
                                         const QString& name,
                                         bool fromCache)
    {
        DevicePropertyProvider* provider = propertyProviderForDevice(cameraId);
        return provider ? provider->getProperty(cameraId, name, fromCache) : QString{};
    }

    bool HardwareRuntime::setProperty(const QString& cameraId,
                                      const QString& name,
                                      const QString& value,
                                      QString* errorMessage)
    {
        DevicePropertyProvider* provider = propertyProviderForDevice(cameraId);
        return provider && provider->setProperty(cameraId, name, value, errorMessage);
    }

    QString HardwareRuntime::getPropertyType(const QString& cameraId, const QString& name)
    {
        DevicePropertyProvider* provider = propertyProviderForDevice(cameraId);
        return provider ? provider->getPropertyType(cameraId, name) : QStringLiteral("Unknown");
    }

    bool HardwareRuntime::isPropertyReadOnly(const QString& cameraId, const QString& name)
    {
        DevicePropertyProvider* provider = propertyProviderForDevice(cameraId);
        return !provider || provider->isPropertyReadOnly(cameraId, name);
    }

    bool HardwareRuntime::isPropertyPreInit(const QString& cameraId, const QString& name)
    {
        DevicePropertyProvider* provider = propertyProviderForDevice(cameraId);
        return provider && provider->isPropertyPreInit(cameraId, name);
    }

    QStringList HardwareRuntime::getAllowedPropertyValues(const QString& cameraId,
                                                          const QString& name)
    {
        DevicePropertyProvider* provider = propertyProviderForDevice(cameraId);
        return provider ? provider->getAllowedPropertyValues(cameraId, name) : QStringList{};
    }

    bool HardwareRuntime::hasPropertyLimits(const QString& cameraId, const QString& name)
    {
        DevicePropertyProvider* provider = propertyProviderForDevice(cameraId);
        return provider && provider->hasPropertyLimits(cameraId, name);
    }

    double HardwareRuntime::getPropertyLowerLimit(const QString& cameraId, const QString& name)
    {
        DevicePropertyProvider* provider = propertyProviderForDevice(cameraId);
        return provider ? provider->getPropertyLowerLimit(cameraId, name) : 0.0;
    }

    double HardwareRuntime::getPropertyUpperLimit(const QString& cameraId, const QString& name)
    {
        DevicePropertyProvider* provider = propertyProviderForDevice(cameraId);
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

    QList<QPair<CameraRuntimeControl*, QStringList>> HardwareRuntime::runtimeControlsFor(
        const QStringList& cameraIds) const
    {
        QHash<CameraRuntimeControl*, QStringList> grouped;
        for (const QString& cameraId : cameraIds)
        {
            const QString normalizedId = cameraId.trimmed();
            const HardwareProviderPtr provider = m_registry.providerForDevice(normalizedId);
            if (auto* control = dynamic_cast<CameraRuntimeControl*>(provider.get()))
            {
                QStringList& providerCameraIds = grouped[control];
                if (!providerCameraIds.contains(normalizedId))
                {
                    providerCameraIds.append(normalizedId);
                }
            }
        }
        QList<QPair<CameraRuntimeControl*, QStringList>> result;
        result.reserve(grouped.size());
        for (auto it = grouped.cbegin(); it != grouped.cend(); ++it)
        {
            result.append({it.key(), it.value()});
        }
        return result;
    }

    void HardwareRuntime::setFrameDeliveryPaused(const QStringList& cameraIds, bool paused)
    {
        for (const auto& [control, providerCameraIds] : runtimeControlsFor(cameraIds))
        {
            control->setFrameDeliveryPaused(providerCameraIds, paused);
        }
    }

    bool HardwareRuntime::setRecordingFrameDeliveryEnabled(const QStringList& cameraIds,
                                                            bool enabled)
    {
        QList<QPair<CameraRuntimeControl*, QStringList>> changed;
        for (const auto& entry : runtimeControlsFor(cameraIds))
        {
            if (!entry.first->setRecordingFrameDeliveryEnabled(entry.second, enabled))
            {
                for (const auto& previous : changed)
                {
                    previous.first->setRecordingFrameDeliveryEnabled(previous.second, !enabled);
                }
                return false;
            }
            changed.append(entry);
        }
        return true;
    }

    bool HardwareRuntime::setHighRateFrameDeliveryEnabled(const QStringList& cameraIds,
                                                           bool enabled)
    {
        QList<QPair<CameraRuntimeControl*, QStringList>> changed;
        for (const auto& entry : runtimeControlsFor(cameraIds))
        {
            if (!entry.first->setHighRateFrameDeliveryEnabled(entry.second, enabled))
            {
                for (const auto& previous : changed)
                {
                    previous.first->setHighRateFrameDeliveryEnabled(previous.second, !enabled);
                }
                return false;
            }
            changed.append(entry);
        }
        return true;
    }

    bool HardwareRuntime::isProcessingFrameTokenCurrent(const QString& cameraId, quint64 token)
    {
        const HardwareProviderPtr provider = m_registry.providerForDevice(cameraId);
        auto* control = dynamic_cast<CameraRuntimeControl*>(provider.get());
        return control && control->isProcessingFrameTokenCurrent(cameraId, token);
    }

    void HardwareRuntime::finishProcessingFrame(const QString& cameraId, quint64 token)
    {
        const HardwareProviderPtr provider = m_registry.providerForDevice(cameraId);
        if (auto* control = dynamic_cast<CameraRuntimeControl*>(provider.get()))
        {
            control->finishProcessingFrame(cameraId, token);
        }
    }

    QString HardwareRuntime::defaultXYStage() const
    {
        QString match;
        for (const HardwareProviderDescriptor& descriptor : m_registry.providers())
        {
            const HardwareProviderPtr provider = m_registry.provider(descriptor.id);
            if (auto* stageProvider = dynamic_cast<StageProvider*>(provider.get()))
            {
                const QString deviceId = stageProvider->defaultXYStage().trimmed();
                const HardwareDeviceDescriptor device = m_registry.device(deviceId);
                if (device.providerId == descriptor.id
                    && device.kind == HardwareDeviceKind::XYStage)
                {
                    if (!match.isEmpty() && match != deviceId)
                    {
                        return {};
                    }
                    match = deviceId;
                }
            }
        }
        return match;
    }

    QString HardwareRuntime::defaultZStage() const
    {
        QString match;
        for (const HardwareProviderDescriptor& descriptor : m_registry.providers())
        {
            const HardwareProviderPtr provider = m_registry.provider(descriptor.id);
            if (auto* stageProvider = dynamic_cast<StageProvider*>(provider.get()))
            {
                const QString deviceId = stageProvider->defaultZStage().trimmed();
                const HardwareDeviceDescriptor device = m_registry.device(deviceId);
                if (device.providerId == descriptor.id
                    && device.kind == HardwareDeviceKind::ZStage)
                {
                    if (!match.isEmpty() && match != deviceId)
                    {
                        return {};
                    }
                    match = deviceId;
                }
            }
        }
        return match;
    }

    bool HardwareRuntime::getXYPosition(const QString& deviceId,
                                        double& x,
                                        double& y,
                                        QString* errorMessage) const
    {
        StageProvider* provider = stageProviderForDevice(deviceId);
        if (!provider)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Stage provider not available");
            }
            return false;
        }
        return provider->getXYPosition(deviceId, x, y, errorMessage);
    }

    bool HardwareRuntime::getZPosition(const QString& deviceId,
                                       double& z,
                                       QString* errorMessage) const
    {
        StageProvider* provider = stageProviderForDevice(deviceId);
        if (!provider)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Stage provider not available");
            }
            return false;
        }
        return provider->getZPosition(deviceId, z, errorMessage);
    }

    bool HardwareRuntime::setRelativeXYPosition(const QString& deviceId,
                                                double dx,
                                                double dy,
                                                QString* errorMessage)
    {
        StageProvider* provider = stageProviderForDevice(deviceId);
        if (!provider)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Stage provider not available");
            }
            return false;
        }
        return provider->setRelativeXYPosition(deviceId, dx, dy, errorMessage);
    }

    bool HardwareRuntime::setRelativeZPosition(const QString& deviceId,
                                               double dz,
                                               QString* errorMessage)
    {
        StageProvider* provider = stageProviderForDevice(deviceId);
        if (!provider)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Stage provider not available");
            }
            return false;
        }
        return provider->setRelativeZPosition(deviceId, dz, errorMessage);
    }

    bool HardwareRuntime::setXYPosition(const QString& deviceId,
                                        double x,
                                        double y,
                                        QString* errorMessage)
    {
        StageProvider* provider = stageProviderForDevice(deviceId);
        if (!provider)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Stage provider not available");
            }
            return false;
        }
        return provider->setXYPosition(deviceId, x, y, errorMessage);
    }

    bool HardwareRuntime::setZPosition(const QString& deviceId,
                                       double z,
                                       QString* errorMessage)
    {
        StageProvider* provider = stageProviderForDevice(deviceId);
        if (!provider)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Stage provider not available");
            }
            return false;
        }
        return provider->setZPosition(deviceId, z, errorMessage);
    }

    bool HardwareRuntime::isShutterOpen(const QString& deviceId,
                                        bool& open,
                                        QString* errorMessage) const
    {
        ShutterProvider* provider = shutterProviderForDevice(deviceId);
        if (!provider)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Shutter provider not available");
            }
            return false;
        }
        return provider->isShutterOpen(deviceId, open, errorMessage);
    }

    bool HardwareRuntime::setShutterOpen(const QString& deviceId,
                                         bool open,
                                         QString* errorMessage)
    {
        ShutterProvider* provider = shutterProviderForDevice(deviceId);
        if (!provider)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Shutter provider not available");
            }
            return false;
        }
        return provider->setShutterOpen(deviceId, open, errorMessage);
    }

    bool HardwareRuntime::getState(const QString& deviceId,
                                   long& state,
                                   QString* errorMessage) const
    {
        StateProvider* provider = stateProviderForDevice(deviceId);
        if (!provider)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("State provider not available");
            }
            return false;
        }
        return provider->getState(deviceId, state, errorMessage);
    }

    bool HardwareRuntime::setState(const QString& deviceId,
                                   long state,
                                   QString* errorMessage)
    {
        StateProvider* provider = stateProviderForDevice(deviceId);
        if (!provider)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("State provider not available");
            }
            return false;
        }
        return provider->setState(deviceId, state, errorMessage);
    }

    QString HardwareRuntime::stateLabel(const QString& deviceId, long state) const
    {
        StateProvider* provider = stateProviderForDevice(deviceId);
        return provider ? provider->stateLabel(deviceId, state) : QString{};
    }

    QStringList HardwareRuntime::availableConfigGroups() const
    {
        QHash<QString, int> groupCounts;
        for (const HardwareProviderDescriptor& descriptor : m_registry.providers())
        {
            const HardwareProviderPtr provider = m_registry.provider(descriptor.id);
            if (auto* configurationProvider = dynamic_cast<ConfigurationProvider*>(provider.get()))
            {
                QSet<QString> providerGroups;
                for (const QString& group : configurationProvider->availableConfigGroups())
                {
                    const QString normalizedGroup = group.trimmed();
                    if (!normalizedGroup.isEmpty())
                    {
                        providerGroups.insert(normalizedGroup);
                    }
                }
                for (const QString& group : providerGroups)
                {
                    ++groupCounts[group];
                }
            }
        }
        QStringList groups;
        for (auto it = groupCounts.cbegin(); it != groupCounts.cend(); ++it)
        {
            if (it.value() == 1)
            {
                groups.append(it.key());
            }
        }
        std::sort(groups.begin(), groups.end());
        return groups;
    }

    QStringList HardwareRuntime::availableConfigs(const QString& groupName) const
    {
        ConfigurationProvider* provider = configurationProviderForGroup(groupName);
        return provider ? provider->availableConfigs(groupName) : QStringList{};
    }

    QString HardwareRuntime::currentConfig(const QString& groupName) const
    {
        ConfigurationProvider* provider = configurationProviderForGroup(groupName);
        return provider ? provider->currentConfig(groupName) : QString{};
    }

    bool HardwareRuntime::setConfig(const QString& groupName,
                                    const QString& configName,
                                    QString* errorMessage)
    {
        ConfigurationProvider* provider = configurationProviderForGroup(groupName);
        if (!provider)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Configuration provider not available");
            }
            return false;
        }
        return provider->setConfig(groupName, configName, errorMessage);
    }

    void HardwareRuntime::clear()
    {
        for (const HardwareProviderDescriptor& descriptor : m_registry.providers())
        {
            const HardwareProviderPtr provider = m_registry.provider(descriptor.id);
            if (auto* cameraProvider = dynamic_cast<CameraProvider*>(provider.get()))
            {
                cameraProvider->stopPreview();
                cameraProvider->setFrameSink({});
                cameraProvider->setPreviewStateSink({});
            }
        }
        m_registry.clear();
    }

    bool HardwareRuntime::registerProvider(const HardwareProviderPtr& provider)
    {
        if (!provider)
        {
            return false;
        }
        const HardwareProviderDescriptor descriptor = provider->descriptor();
        if (descriptor.id.trimmed().isEmpty())
        {
            return false;
        }
        const HardwareProviderPtr previous = m_registry.provider(descriptor.id);
        if (previous && previous != provider)
        {
            return false;
        }
        auto* cameraProvider = dynamic_cast<CameraProvider*>(provider.get());
        auto* stageProvider = dynamic_cast<StageProvider*>(provider.get());
        auto* shutterProvider = dynamic_cast<ShutterProvider*>(provider.get());
        auto* stateProvider = dynamic_cast<StateProvider*>(provider.get());
        const QList<HardwareDeviceDescriptor> providerDevices = provider->devices();
        for (const HardwareDeviceDescriptor& device : providerDevices)
        {
            const bool supported =
                (device.kind != HardwareDeviceKind::Camera || cameraProvider)
                && ((device.kind != HardwareDeviceKind::XYStage
                     && device.kind != HardwareDeviceKind::ZStage)
                    || stageProvider)
                && (device.kind != HardwareDeviceKind::Shutter || shutterProvider)
                && (device.kind != HardwareDeviceKind::State || stateProvider);
            if (!supported)
            {
                return false;
            }
        }
        if (!m_registry.registerProvider(provider, descriptor, providerDevices))
        {
            return false;
        }
        if (cameraProvider)
        {
            cameraProvider->setFrameSink([this](const ImageFrame& frame)
            {
                m_frameRouter.publish(frame);
            });
            cameraProvider->setPreviewStateSink([this](bool)
            {
                const auto publishState = [this]()
                {
                    bool running = false;
                    for (const HardwareDeviceDescriptor& device : m_registry.devices())
                    {
                        if (device.kind == HardwareDeviceKind::Camera
                            && isPreviewRunning(device.logicalId))
                        {
                            running = true;
                            break;
                        }
                    }
                    if (m_previewStateSink) m_previewStateSink(running);
                    emit previewStateChanged(running);
                };
                if (QThread::currentThread() == thread()) publishState();
                else QMetaObject::invokeMethod(this, publishState, Qt::QueuedConnection);
            });
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
            cameraProvider->setPreviewStateSink({});
        }
        m_registry.unregisterProvider(providerId);
    }

    bool HardwareRuntime::refreshProvider(const QString& providerId)
    {
        const HardwareProviderPtr provider = m_registry.provider(providerId);
        return provider && registerProvider(provider);
    }

    bool HardwareRuntime::stopPreviewForProvider(const QString& providerId)
    {
        const HardwareProviderPtr provider = m_registry.provider(providerId);
        auto* cameraProvider = dynamic_cast<CameraProvider*>(provider.get());
        return cameraProvider && cameraProvider->stopPreview();
    }
}

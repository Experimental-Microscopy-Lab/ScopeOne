#include "internal/HardwareRuntime.h"

#include "internal/DaqDeviceManager.h"
#include "internal/DriverHostClient.h"
#include "internal/SignalSourceManager.h"
#include "scopeone/Camera.h"

#include <algorithm>
#include <QMetaObject>
#include <functional>
#include <QSet>
#include <QThread>
#include <utility>

namespace scopeone::core::internal
{
    namespace
    {
        // Calls through to a provider, or reports that nothing serves the request
        template <class Provider, class Call>
        bool forward(Provider* provider, const char* missing, QString* errorMessage, Call&& call)
        {
            if (!provider)
            {
                if (errorMessage)
                {
                    *errorMessage = QString::fromLatin1(missing);
                }
                return false;
            }
            return call(*provider);
        }

        // The device of the given kind that exactly one provider names as its default, such as
        // the focus stage or the shutter, and that provider. None when two providers disagree.
        template <class Provider>
        Provider* uniqueDefault(const DeviceRegistry& registry,
                                QString (Provider::*defaultDevice)() const,
                                HardwareDeviceKind kind,
                                QString* deviceId)
        {
            Provider* match = nullptr;
            QString matchId;
            for (const HardwareProviderDescriptor& descriptor : registry.providers())
            {
                auto* provider = dynamic_cast<Provider*>(registry.provider(descriptor.id).get());
                const QString id = provider ? (provider->*defaultDevice)().trimmed() : QString{};
                const HardwareDeviceDescriptor device = registry.device(id);
                if (id.isEmpty() || device.providerId != descriptor.id || device.kind != kind)
                {
                    continue;
                }
                if (match)
                {
                    return nullptr;
                }
                match = provider;
                matchId = id;
            }
            *deviceId = matchId;
            return match;
        }

        Shutter* defaultShutter(const DeviceRegistry& registry, QString* shutter)
        {
            return uniqueDefault(registry, &Shutter::defaultShutter, HardwareDeviceKind::Shutter, shutter);
        }

        Autofocus* defaultAutofocus(const DeviceRegistry& registry, QString* device)
        {
            return uniqueDefault(registry, &Autofocus::defaultAutofocus, HardwareDeviceKind::AutoFocus, device);
        }

        void stopCameras(const DeviceRegistry& registry, const QString& providerId)
        {
            const DevicePtr provider = registry.provider(providerId);
            auto* cameraProvider = dynamic_cast<Camera*>(provider.get());
            if (!cameraProvider)
            {
                return;
            }
            for (const HardwareDeviceDescriptor& device : registry.devices())
            {
                if (device.providerId == providerId && device.kind == HardwareDeviceKind::Camera)
                {
                    cameraProvider->stopPreviewFor(device.logicalId);
                }
            }
        }

        void detachCamera(const DeviceRegistry& registry, const QString& providerId)
        {
            stopCameras(registry, providerId);
            const DevicePtr provider = registry.provider(providerId);
            if (auto* cameraProvider = dynamic_cast<Camera*>(provider.get()))
            {
                cameraProvider->setPreviewStateSink({});
            }
            if (auto* frameSource = dynamic_cast<CameraFrameSource*>(provider.get()))
            {
                frameSource->setFrameDelivery(nullptr);
            }
        }
    }

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
        const DevicePtr& provider,
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

    DevicePtr DeviceRegistry::provider(const QString& providerId) const
    {
        QReadLocker locker(&m_lock);
        const auto it = m_providers.constFind(providerId.trimmed());
        return it == m_providers.constEnd() ? DevicePtr{} : it->provider;
    }

    DevicePtr DeviceRegistry::providerForDevice(const QString& logicalId) const
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
          , m_microManager(std::make_shared<MicroManagerProvider>())
          , m_acquisitionEngine(m_registry)
    {
        connect(&m_registry, &DeviceRegistry::changed,
                this, &HardwareRuntime::devicesChanged);
        connect(&m_frameDelivery, &CameraFrameDelivery::frameReady,
                this, &HardwareRuntime::frameReady);
        m_signalSourceManager = new SignalSourceManager(&m_registry, this);
        m_daqDeviceManager = new DaqDeviceManager(&m_registry, this);
        registerProvider(m_microManager);
    }

    Camera* HardwareRuntime::cameraForDevice(const QString& logicalId) const
    {
        const DevicePtr provider = m_registry.providerForDevice(logicalId);
        return dynamic_cast<Camera*>(provider.get());
    }

    Device* HardwareRuntime::propertyDeviceForDevice(
        const QString& logicalId) const
    {
        const DevicePtr provider = m_registry.providerForDevice(logicalId);
        return dynamic_cast<Device*>(provider.get());
    }

    Stage* HardwareRuntime::stageForDevice(const QString& logicalId) const
    {
        const DevicePtr provider = m_registry.providerForDevice(logicalId);
        return dynamic_cast<Stage*>(provider.get());
    }

    Shutter* HardwareRuntime::shutterForDevice(const QString& logicalId) const
    {
        const DevicePtr provider = m_registry.providerForDevice(logicalId);
        return dynamic_cast<Shutter*>(provider.get());
    }

    StateDevice* HardwareRuntime::stateDeviceForDevice(const QString& logicalId) const
    {
        const DevicePtr provider = m_registry.providerForDevice(logicalId);
        return dynamic_cast<StateDevice*>(provider.get());
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
            const DevicePtr provider = m_registry.provider(descriptor.id);
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
        Camera* provider = cameraForDevice(cameraId);
        return provider && provider->isPreviewRunning(cameraId);
    }

    bool HardwareRuntime::getExposure(const QString& cameraIdOrAll, double& exposureMs) const
    {
        const QString target = cameraIdOrAll.trimmed();
        if (target.compare(QStringLiteral("All"), Qt::CaseInsensitive) != 0)
        {
            Camera* provider = cameraForDevice(target);
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
            Camera* provider = cameraForDevice(device.logicalId);
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
            Camera* provider = cameraForDevice(target);
            return provider && provider->setExposure(target, exposureMs);
        }
        bool ok = false;
        bool all = true;
        for (const HardwareDeviceDescriptor& device : m_registry.devices())
        {
            if (device.kind != HardwareDeviceKind::Camera)
            {
                continue;
            }
            Camera* provider = cameraForDevice(device.logicalId);
            ok = true;
            all = provider && provider->setExposure(device.logicalId, exposureMs) && all;
        }
        return ok && all;
    }

    QStringList HardwareRuntime::listProperties(const QString& cameraId)
    {
        Device* provider = propertyDeviceForDevice(cameraId);
        return provider ? provider->listProperties(cameraId) : QStringList{};
    }

    QString HardwareRuntime::getProperty(const QString& cameraId,
                                         const QString& name,
                                         bool fromCache)
    {
        Device* provider = propertyDeviceForDevice(cameraId);
        return provider ? provider->getProperty(cameraId, name, fromCache) : QString{};
    }

    bool HardwareRuntime::setProperty(const QString& cameraId,
                                      const QString& name,
                                      const QString& value,
                                      QString* errorMessage)
    {
        Device* provider = propertyDeviceForDevice(cameraId);
        return provider && provider->setProperty(cameraId, name, value, errorMessage);
    }

    QString HardwareRuntime::getPropertyType(const QString& cameraId, const QString& name)
    {
        Device* provider = propertyDeviceForDevice(cameraId);
        return provider ? provider->getPropertyType(cameraId, name) : QStringLiteral("Unknown");
    }

    bool HardwareRuntime::isPropertyReadOnly(const QString& cameraId, const QString& name)
    {
        Device* provider = propertyDeviceForDevice(cameraId);
        return !provider || provider->isPropertyReadOnly(cameraId, name);
    }

    bool HardwareRuntime::isPropertyPreInit(const QString& cameraId, const QString& name)
    {
        Device* provider = propertyDeviceForDevice(cameraId);
        return provider && provider->isPropertyPreInit(cameraId, name);
    }

    QStringList HardwareRuntime::getAllowedPropertyValues(const QString& cameraId,
                                                          const QString& name)
    {
        Device* provider = propertyDeviceForDevice(cameraId);
        return provider ? provider->getAllowedPropertyValues(cameraId, name) : QStringList{};
    }

    bool HardwareRuntime::hasPropertyLimits(const QString& cameraId, const QString& name)
    {
        Device* provider = propertyDeviceForDevice(cameraId);
        return provider && provider->hasPropertyLimits(cameraId, name);
    }

    double HardwareRuntime::getPropertyLowerLimit(const QString& cameraId, const QString& name)
    {
        Device* provider = propertyDeviceForDevice(cameraId);
        return provider ? provider->getPropertyLowerLimit(cameraId, name) : 0.0;
    }

    double HardwareRuntime::getPropertyUpperLimit(const QString& cameraId, const QString& name)
    {
        Device* provider = propertyDeviceForDevice(cameraId);
        return provider ? provider->getPropertyUpperLimit(cameraId, name) : 0.0;
    }

    bool HardwareRuntime::setROI(const QString& cameraId,
                                 int x,
                                 int y,
                                 int width,
                                 int height)
    {
        Camera* provider = cameraForDevice(cameraId);
        return provider && provider->setROI(cameraId, x, y, width, height);
    }

    bool HardwareRuntime::clearROI(const QString& cameraId)
    {
        Camera* provider = cameraForDevice(cameraId);
        return provider && provider->clearROI(cameraId);
    }

    bool HardwareRuntime::getROI(const QString& cameraId,
                                 int& x,
                                 int& y,
                                 int& width,
                                 int& height)
    {
        Camera* provider = cameraForDevice(cameraId);
        return provider && provider->getROI(cameraId, x, y, width, height);
    }

    bool HardwareRuntime::captureEventFrame(const QString& cameraId,
                                            ImageFrame& frame,
                                            int timeoutMs)
    {
        Camera* provider = cameraForDevice(cameraId);
        return provider && provider->captureEventFrame(cameraId, frame, timeoutMs);
    }

    bool HardwareRuntime::supportsEventSequence(const QString& cameraId) const
    {
        Camera* provider = cameraForDevice(cameraId);
        return provider && provider->supportsEventSequence(cameraId);
    }

    bool HardwareRuntime::captureEventSequence(const QString& cameraId,
                                               int count,
                                               std::vector<ImageFrame>& frames,
                                               int timeoutMs)
    {
        Camera* provider = cameraForDevice(cameraId);
        return provider && provider->captureEventSequence(cameraId, count, frames, timeoutMs);
    }

    QList<QPair<CameraFrameSource*, QStringList>> HardwareRuntime::frameSourcesFor(
        const QStringList& cameraIds) const
    {
        QHash<CameraFrameSource*, QStringList> grouped;
        for (const QString& cameraId : cameraIds)
        {
            const QString normalizedId = cameraId.trimmed();
            const DevicePtr provider = m_registry.providerForDevice(normalizedId);
            if (auto* source = dynamic_cast<CameraFrameSource*>(provider.get()))
            {
                QStringList& providerCameraIds = grouped[source];
                if (!providerCameraIds.contains(normalizedId))
                {
                    providerCameraIds.append(normalizedId);
                }
            }
        }
        QList<QPair<CameraFrameSource*, QStringList>> result;
        for (auto it = grouped.cbegin(); it != grouped.cend(); ++it)
        {
            result.append({it.key(), it.value()});
        }
        return result;
    }

    // Switches each camera to its mode, and every camera back when one provider refuses
    bool HardwareRuntime::applyFrameDeliveryModes(const QHash<QString, FrameDeliveryMode>& modes)
    {
        const auto apply = [this](const QStringList& cameraIds,
                                  const std::function<FrameDeliveryMode(const QString&)>& modeOf)
        {
            for (const auto& [source, providerCameraIds] : frameSourcesFor(cameraIds))
            {
                for (const FrameDeliveryMode mode : {FrameDeliveryMode::PreviewLatest,
                                                     FrameDeliveryMode::LatestOnly,
                                                     FrameDeliveryMode::AllFrames})
                {
                    QStringList ids;
                    for (const QString& cameraId : providerCameraIds)
                    {
                        if (modeOf(cameraId) == mode) ids.append(cameraId);
                    }
                    if (!ids.isEmpty() && !source->setFrameDeliveryMode(ids, mode))
                    {
                        return false;
                    }
                }
            }
            return true;
        };
        if (apply(modes.keys(), [&modes](const QString& cameraId) { return modes.value(cameraId); }))
        {
            return true;
        }
        apply(modes.keys(), [this](const QString& cameraId) { return m_frameDelivery.mode(cameraId); });
        return false;
    }

    void HardwareRuntime::setFrameDeliveryPaused(const QStringList& cameraIds, bool paused)
    {
        for (const auto& [source, providerCameraIds] : frameSourcesFor(cameraIds))
        {
            source->setFrameDeliveryPaused(providerCameraIds, paused);
        }
    }

    // The cameras deliver every frame before recording takes them, and recording lets go of
    // them before the cameras skip frames again
    bool HardwareRuntime::setRecordingFrameDeliveryEnabled(const QStringList& cameraIds, bool enabled)
    {
        QHash<QString, FrameDeliveryMode> modes;
        for (const QString& cameraId : cameraIds)
        {
            modes.insert(cameraId, enabled ? FrameDeliveryMode::AllFrames
                                           : m_frameDelivery.highRateEnabled(cameraId)
                                                 ? FrameDeliveryMode::LatestOnly
                                                 : FrameDeliveryMode::PreviewLatest);
        }
        if (enabled)
        {
            if (!applyFrameDeliveryModes(modes))
            {
                return false;
            }
            m_frameDelivery.setRecordingEnabled(cameraIds, true);
            return true;
        }
        m_frameDelivery.setRecordingEnabled(cameraIds, false);
        applyFrameDeliveryModes(modes);
        return true;
    }

    bool HardwareRuntime::setHighRateFrameDeliveryEnabled(const QStringList& cameraIds, bool enabled)
    {
        QHash<QString, FrameDeliveryMode> modes;
        for (const QString& cameraId : cameraIds)
        {
            modes.insert(cameraId, m_frameDelivery.recordingEnabled(cameraId)
                                       ? FrameDeliveryMode::AllFrames
                                       : enabled ? FrameDeliveryMode::LatestOnly : FrameDeliveryMode::PreviewLatest);
        }
        if (!applyFrameDeliveryModes(modes))
        {
            return false;
        }
        m_frameDelivery.setHighRateEnabled(cameraIds, enabled);
        return true;
    }

    bool HardwareRuntime::isProcessingFrameTokenCurrent(const QString& cameraId, quint64 token)
    {
        return m_frameDelivery.isProcessingFrameCurrent(cameraId, token);
    }

    void HardwareRuntime::finishProcessingFrame(const QString& cameraId, quint64 token)
    {
        m_frameDelivery.finishProcessingFrame(cameraId, token);
    }

    QString HardwareRuntime::defaultXYStage() const
    {
        QString stage;
        uniqueDefault(m_registry, &Stage::defaultXYStage, HardwareDeviceKind::XYStage, &stage);
        return stage;
    }

    QString HardwareRuntime::defaultZStage() const
    {
        QString stage;
        uniqueDefault(m_registry, &Stage::defaultZStage, HardwareDeviceKind::ZStage, &stage);
        return stage;
    }

    bool HardwareRuntime::getXYPosition(const QString& deviceId,
                                        double& x,
                                        double& y,
                                        QString* errorMessage) const
    {
        return forward(stageForDevice(deviceId), "Stage provider not available", errorMessage,
                       [&](Stage& provider)
                       {
                           return provider.getXYPosition(deviceId, x, y, errorMessage);
                       });
    }

    bool HardwareRuntime::getZPosition(const QString& deviceId,
                                       double& z,
                                       QString* errorMessage) const
    {
        return forward(stageForDevice(deviceId), "Stage provider not available", errorMessage,
                       [&](Stage& provider)
                       {
                           return provider.getZPosition(deviceId, z, errorMessage);
                       });
    }

    bool HardwareRuntime::setRelativeXYPosition(const QString& deviceId,
                                                double dx,
                                                double dy,
                                                QString* errorMessage)
    {
        return forward(stageForDevice(deviceId), "Stage provider not available", errorMessage,
                       [&](Stage& provider)
                       {
                           return provider.setRelativeXYPosition(deviceId, dx, dy, errorMessage);
                       });
    }

    bool HardwareRuntime::setRelativeZPosition(const QString& deviceId,
                                               double dz,
                                               QString* errorMessage)
    {
        return forward(stageForDevice(deviceId), "Stage provider not available", errorMessage,
                       [&](Stage& provider)
                       {
                           return provider.setRelativeZPosition(deviceId, dz, errorMessage);
                       });
    }

    bool HardwareRuntime::setXYPosition(const QString& deviceId,
                                        double x,
                                        double y,
                                        QString* errorMessage)
    {
        return forward(stageForDevice(deviceId), "Stage provider not available", errorMessage,
                       [&](Stage& provider)
                       {
                           return provider.setXYPosition(deviceId, x, y, errorMessage);
                       });
    }

    bool HardwareRuntime::setZPosition(const QString& deviceId,
                                       double z,
                                       QString* errorMessage)
    {
        return forward(stageForDevice(deviceId), "Stage provider not available", errorMessage,
                       [&](Stage& provider)
                       {
                           return provider.setZPosition(deviceId, z, errorMessage);
                       });
    }

    bool HardwareRuntime::isShutterOpen(const QString& deviceId,
                                        bool& open,
                                        QString* errorMessage) const
    {
        return forward(shutterForDevice(deviceId), "Shutter provider not available", errorMessage,
                       [&](Shutter& provider)
                       {
                           return provider.isShutterOpen(deviceId, open, errorMessage);
                       });
    }

    bool HardwareRuntime::setShutterOpen(const QString& deviceId,
                                         bool open,
                                         QString* errorMessage)
    {
        return forward(shutterForDevice(deviceId), "Shutter provider not available", errorMessage,
                       [&](Shutter& provider)
                       {
                           return provider.setShutterOpen(deviceId, open, errorMessage);
                       });
    }

    bool HardwareRuntime::getState(const QString& deviceId,
                                   long& state,
                                   QString* errorMessage) const
    {
        return forward(stateDeviceForDevice(deviceId), "State provider not available", errorMessage,
                       [&](StateDevice& provider)
                       {
                           return provider.getState(deviceId, state, errorMessage);
                       });
    }

    bool HardwareRuntime::setState(const QString& deviceId,
                                   long state,
                                   QString* errorMessage)
    {
        return forward(stateDeviceForDevice(deviceId), "State provider not available", errorMessage,
                       [&](StateDevice& provider)
                       {
                           return provider.setState(deviceId, state, errorMessage);
                       });
    }

    QString HardwareRuntime::stateLabel(const QString& deviceId, long state) const
    {
        StateDevice* provider = stateDeviceForDevice(deviceId);
        return provider ? provider->stateLabel(deviceId, state) : QString{};
    }

    QStringList HardwareRuntime::availableConfigGroups() const
    {
        QHash<QString, int> groupCounts;
        for (const HardwareProviderDescriptor& descriptor : m_registry.providers())
        {
            const DevicePtr provider = m_registry.provider(descriptor.id);
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

    std::vector<scopeone::core::ConfigSetting> HardwareRuntime::configSettings(
        const QString& groupName, const QString& configName) const
    {
        ConfigurationProvider* provider = configurationProviderForGroup(groupName);
        return provider ? provider->configSettings(groupName, configName)
                        : std::vector<scopeone::core::ConfigSetting>{};
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
        return forward(configurationProviderForGroup(groupName), "Configuration provider not available",
                       errorMessage,
                       [&](ConfigurationProvider& provider)
                       {
                           return provider.setConfig(groupName, configName, errorMessage);
                       });
    }

    bool HardwareRuntime::autoShutter(bool& enabled, QString* errorMessage) const
    {
        QString shutter;
        Shutter* const provider = defaultShutter(m_registry, &shutter);
        enabled = provider && provider->autoShutter();
        if (!provider && errorMessage)
        {
            *errorMessage = QStringLiteral("No shutter lights the cameras");
        }
        return provider != nullptr;
    }

    bool HardwareRuntime::setAutoShutter(bool enabled, QString* errorMessage)
    {
        QString shutter;
        return forward(defaultShutter(m_registry, &shutter), "No shutter lights the cameras", errorMessage,
                       [&](Shutter& provider) { return provider.setAutoShutter(enabled, errorMessage); });
    }

    bool HardwareRuntime::setDefaultShutterOpen(bool open, QString* errorMessage)
    {
        QString shutter;
        return forward(defaultShutter(m_registry, &shutter), "No shutter lights the cameras", errorMessage,
                       [&](Shutter& provider) { return provider.setShutterOpen(shutter, open, errorMessage); });
    }

    bool HardwareRuntime::hasAutofocus() const
    {
        QString device;
        return defaultAutofocus(m_registry, &device) != nullptr;
    }

    bool HardwareRuntime::runAutofocus(QString* errorMessage)
    {
        QString device;
        return forward(defaultAutofocus(m_registry, &device), "Autofocus is not available", errorMessage,
                       [&](Autofocus& provider) { return provider.runAutofocus(device, errorMessage); });
    }

    long HardwareRuntime::focusSequenceCapacity(const QString& focusDevice) const
    {
        Stage* provider = stageForDevice(focusDevice);
        return provider ? provider->stageSequenceCapacity(focusDevice) : 0;
    }

    bool HardwareRuntime::startFocusSequence(const QString& focusDevice,
                                             const std::vector<double>& positions,
                                             QString* errorMessage)
    {
        return forward(stageForDevice(focusDevice), "Stage provider not available", errorMessage,
                       [&](Stage& provider)
                       {
                           return provider.startStageSequence(focusDevice, positions, errorMessage);
                       });
    }

    void HardwareRuntime::stopFocusSequence(const QString& focusDevice)
    {
        if (Stage* provider = stageForDevice(focusDevice))
        {
            provider->stopStageSequence(focusDevice);
        }
    }

    long HardwareRuntime::propertySequenceCapacity(const QString& device, const QString& property) const
    {
        Device* provider = propertyDeviceForDevice(device);
        return provider ? provider->propertySequenceCapacity(device, property) : 0;
    }

    // Nothing stays armed when one of the sequences fails
    bool HardwareRuntime::startPropertySequences(const std::vector<PropertySequence>& sequences,
                                                 QString* errorMessage)
    {
        for (std::size_t index = 0; index < sequences.size(); ++index)
        {
            const PropertySequence& sequence = sequences[index];
            Device* provider = propertyDeviceForDevice(sequence.device);
            const QStringList values(sequence.values.begin(), sequence.values.end());
            if (!provider || !provider->startPropertySequence(sequence.device, sequence.property, values, errorMessage))
            {
                stopPropertySequences({sequences.begin(), sequences.begin() + static_cast<std::ptrdiff_t>(index)});
                return false;
            }
        }
        return true;
    }

    void HardwareRuntime::stopPropertySequences(const std::vector<PropertySequence>& sequences)
    {
        for (const PropertySequence& sequence : sequences)
        {
            if (Device* provider = propertyDeviceForDevice(sequence.device))
            {
                provider->stopPropertySequence(sequence.device, sequence.property);
            }
        }
    }

    QString HardwareRuntime::microManagerVersion()
    {
        return MicroManagerProvider::coreVersion();
    }

    double HardwareRuntime::pixelSizeUm(const QString& cameraId) const
    {
        Camera* provider = cameraForDevice(cameraId);
        return provider ? provider->pixelSizeUm(cameraId) : 0.0;
    }

    DevicePtr HardwareRuntime::startScopeOneProvider(const QString& providerId,
                                                     const QString& driverPath,
                                                     const QString& deviceName,
                                                     const QVariantMap& preInitProperties,
                                                     QString* errorMessage)
    {
        return createScopeOneProvider(providerId, driverPath, deviceName, preInitProperties, errorMessage);
    }

    QStringList HardwareRuntime::microManagerAdapterSearchPaths() const
    {
        return m_microManager->adapterSearchPaths();
    }

    void HardwareRuntime::setMicroManagerAdapterSearchPaths(const QStringList& paths)
    {
        m_microManager->setAdapterSearchPaths(paths);
    }

    bool HardwareRuntime::loadMicroManagerConfiguration(const QString& configPath,
                                                        MicroManagerProvider::LoadResult& result,
                                                        QString& errorMessage)
    {
        return m_microManager->loadConfiguration(configPath, result, errorMessage);
    }

    QStringList HardwareRuntime::startMicroManagerCameras(const QList<MicroManagerCameraSetup>& cameras)
    {
        return m_microManager->startCameras(cameras);
    }

    QList<DevicePtr> HardwareRuntime::takeMicroManagerCameras()
    {
        return m_microManager->takeCameras();
    }

    bool HardwareRuntime::setMicroManagerDevices(const QList<HardwareDeviceDescriptor>& devices)
    {
        m_microManager->setDevices(devices);
        return registerProvider(m_microManager);
    }

    void HardwareRuntime::stopMicroManagerPreview()
    {
        stopPreviewForProvider(m_microManager->descriptor().id);
    }

    bool HardwareRuntime::unloadMicroManagerDevices(QString* errorMessage)
    {
        return m_microManager->unloadDevices(errorMessage);
    }

    void HardwareRuntime::applyMicroManagerShutdownPreset()
    {
        stopPreviewForProvider(m_microManager->descriptor().id);
        m_microManager->applyShutdownPreset();
    }

    // Removes the drivers. Micro-Manager stays registered, as the runtime owns it for its lifetime.
    void HardwareRuntime::clear()
    {
        for (const HardwareProviderDescriptor& descriptor : m_registry.providers())
        {
            if (descriptor.id == m_microManager->descriptor().id)
            {
                stopCameras(m_registry, descriptor.id);
                continue;
            }
            unregisterProvider(descriptor.id);
        }
    }

    bool HardwareRuntime::registerProvider(const DevicePtr& provider)
    {
        if (!provider)
        {
            return false;
        }
        const HardwareProviderDescriptor descriptor = provider->descriptor();
        const DevicePtr previous = m_registry.provider(descriptor.id);
        if (previous && previous != provider)
        {
            return false;
        }
        auto* cameraProvider = dynamic_cast<Camera*>(provider.get());
        auto* stageProvider = dynamic_cast<Stage*>(provider.get());
        auto* shutterProvider = dynamic_cast<Shutter*>(provider.get());
        auto* stateProvider = dynamic_cast<StateDevice*>(provider.get());
        const QList<HardwareDeviceDescriptor> providerDevices = provider->devices();
        for (const HardwareDeviceDescriptor& device : providerDevices)
        {
            const bool supported =
                (device.kind != HardwareDeviceKind::Camera || cameraProvider)
                && ((device.kind != HardwareDeviceKind::XYStage
                     && device.kind != HardwareDeviceKind::ZStage)
                    || stageProvider)
                && (device.kind != HardwareDeviceKind::Shutter || shutterProvider)
                && (device.kind != HardwareDeviceKind::State || stateProvider)
                && (device.kind != HardwareDeviceKind::AutoFocus
                    || dynamic_cast<Autofocus*>(provider.get()));
            if (!supported)
            {
                return false;
            }
        }
        if (!m_registry.registerProvider(provider, descriptor, providerDevices))
        {
            return false;
        }
        if (auto* frameSource = dynamic_cast<CameraFrameSource*>(provider.get()))
        {
            // The cameras of a newly registered provider start in the live view mode
            QStringList cameraIds;
            for (const HardwareDeviceDescriptor& device : providerDevices)
            {
                if (device.kind == HardwareDeviceKind::Camera)
                {
                    cameraIds.append(device.logicalId);
                }
            }
            m_frameDelivery.removeCameras(cameraIds);
            frameSource->setFrameDelivery(&m_frameDelivery);
        }
        if (cameraProvider)
        {
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
        detachCamera(m_registry, providerId);
        m_registry.unregisterProvider(providerId);
    }

    bool HardwareRuntime::refreshProvider(const QString& providerId)
    {
        const DevicePtr provider = m_registry.provider(providerId);
        return provider && registerProvider(provider);
    }

    void HardwareRuntime::stopPreviewForProvider(const QString& providerId)
    {
        stopCameras(m_registry, providerId);
    }
}

#include "internal/HardwareRuntime.h"

#include "internal/DaqDeviceManager.h"
#include "internal/SignalSourceManager.h"
#include "scopeone/Camera.h"
#include "scopeone/HardwareConfiguration.h"

#include <algorithm>
#include <QDebug>
#include <QMetaObject>
#include <functional>
#include <QSet>
#include <QThread>
#include <QFileInfo>
#include <utility>

namespace scopeone::core::internal
{
    namespace
    {
        bool sameSettings(const std::vector<scopeone::core::ConfigSetting>& a,
                          const std::vector<scopeone::core::ConfigSetting>& b)
        {
            return !a.empty() && a.size() == b.size()
                && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y)
                              { return x.device == y.device && x.property == y.property && x.value == y.value; });
        }

        // Property values compare as numbers when both are, so 1 and 1.000 are the same setting
        bool propertyValuesEqual(const QString& a, const QString& b)
        {
            bool aIsNumber = false;
            bool bIsNumber = false;
            const double x = a.toDouble(&aIsNumber);
            const double y = b.toDouble(&bIsNumber);
            return aIsNumber && bIsNumber ? x == y : a == b;
        }

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
                    cameraProvider->stopPreview(device.logicalId);
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
        if (logicalId.trimmed() == QStringLiteral("Core")) return m_microManager.get();
        const DevicePtr provider = m_registry.providerForDevice(logicalId);
        return dynamic_cast<Device*>(provider.get());
    }

    XYStage* HardwareRuntime::xyStageForDevice(const QString& logicalId) const
    {
        const DevicePtr provider = m_registry.providerForDevice(logicalId);
        return dynamic_cast<XYStage*>(provider.get());
    }

    ZStage* HardwareRuntime::zStageForDevice(const QString& logicalId) const
    {
        const DevicePtr provider = m_registry.providerForDevice(logicalId);
        return dynamic_cast<ZStage*>(provider.get());
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

    // AutoShutter as in MMCore, the shutter of the Shutter role is open while a camera takes images
    bool HardwareRuntime::startPreview(const QString& cameraIdOrAll)
    {
        const bool openShutter = m_autoShutter && !anyCameraRunning();
        if (openShutter) setDefaultShutterOpen(true);
        const bool started = m_acquisitionEngine.start(cameraIdOrAll);
        if (openShutter && !started) setDefaultShutterOpen(false);
        return started;
    }

    bool HardwareRuntime::stopPreview(const QString& cameraIdOrAll)
    {
        return m_acquisitionEngine.stop(cameraIdOrAll);
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

    bool HardwareRuntime::snap(const QString& cameraId,
                               ImageFrame& frame,
                               int timeoutMs)
    {
        Camera* provider = cameraForDevice(cameraId);
        if (!provider) return false;
        const bool openShutter = m_autoShutter && !anyCameraRunning();
        if (openShutter) setDefaultShutterOpen(true);
        const bool ok = provider->snap(cameraId, frame, timeoutMs);
        if (openShutter) setDefaultShutterOpen(false);
        return ok;
    }

    bool HardwareRuntime::supportsSequence(const QString& cameraId) const
    {
        Camera* provider = cameraForDevice(cameraId);
        return provider && provider->supportsSequence(cameraId);
    }

    bool HardwareRuntime::snapSequence(const QString& cameraId,
                                       int count,
                                       std::vector<ImageFrame>& frames,
                                       int timeoutMs)
    {
        Camera* provider = cameraForDevice(cameraId);
        if (!provider) return false;
        const bool openShutter = m_autoShutter && !anyCameraRunning();
        if (openShutter) setDefaultShutterOpen(true);
        const bool ok = provider->snapSequence(cameraId, count, frames, timeoutMs);
        if (openShutter) setDefaultShutterOpen(false);
        return ok;
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
        return roleDevice(QStringLiteral("XYStage"), m_xyStageRole, HardwareDeviceKind::XYStage);
    }

    QString HardwareRuntime::defaultZStage() const
    {
        return roleDevice(QStringLiteral("Focus"), m_focusRole, HardwareDeviceKind::ZStage);
    }

    bool HardwareRuntime::getXYPosition(const QString& deviceId,
                                        double& x,
                                        double& y,
                                        QString* errorMessage) const
    {
        return forward(xyStageForDevice(deviceId), "XY stage provider not available", errorMessage,
                       [&](XYStage& provider)
                       {
                           return provider.getXYPosition(deviceId, x, y, errorMessage);
                       });
    }

    bool HardwareRuntime::getZPosition(const QString& deviceId,
                                       double& z,
                                       QString* errorMessage) const
    {
        return forward(zStageForDevice(deviceId), "Z stage provider not available", errorMessage,
                       [&](ZStage& provider)
                       {
                           return provider.getZPosition(deviceId, z, errorMessage);
                       });
    }

    bool HardwareRuntime::setRelativeXYPosition(const QString& deviceId,
                                                double dx,
                                                double dy,
                                                QString* errorMessage)
    {
        return forward(xyStageForDevice(deviceId), "XY stage provider not available", errorMessage,
                       [&](XYStage& provider)
                       {
                           return provider.setRelativeXYPosition(deviceId, dx, dy, errorMessage);
                       });
    }

    bool HardwareRuntime::setRelativeZPosition(const QString& deviceId,
                                               double dz,
                                               QString* errorMessage)
    {
        return forward(zStageForDevice(deviceId), "Z stage provider not available", errorMessage,
                       [&](ZStage& provider)
                       {
                           return provider.setRelativeZPosition(deviceId, dz, errorMessage);
                       });
    }

    bool HardwareRuntime::setXYPosition(const QString& deviceId,
                                        double x,
                                        double y,
                                        QString* errorMessage)
    {
        return forward(xyStageForDevice(deviceId), "XY stage provider not available", errorMessage,
                       [&](XYStage& provider)
                       {
                           return provider.setXYPosition(deviceId, x, y, errorMessage);
                       });
    }

    bool HardwareRuntime::setZPosition(const QString& deviceId,
                                       double z,
                                       QString* errorMessage)
    {
        return forward(zStageForDevice(deviceId), "Z stage provider not available", errorMessage,
                       [&](ZStage& provider)
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

    // The ScopeOne lines of the cfg file for the devices that are loaded. A device that failed to
    // load or is unloaded takes its lines out of the presets.
    std::vector<ScopeOneConfigGroupLine> HardwareRuntime::scopeOneConfigLines() const
    {
        std::vector<ScopeOneConfigGroupLine> lines;
        for (const ScopeOneConfigGroupLine& line : m_scopeOne.configuration().configGroups)
        {
            if (m_registry.providerForDevice(line.label))
            {
                lines.push_back(line);
            }
        }
        return lines;
    }

    // The light source channels of the cfg file whose devices are all loaded, in cfg order. A
    // channel with a device that failed to load or is unloaded is left out.
    std::vector<LightSourceLine> HardwareRuntime::lightSources() const
    {
        const auto loaded = [this](const QString& device, const QString& property)
        { return property.isEmpty() || m_registry.providerForDevice(device); };
        std::vector<LightSourceLine> sources;
        for (const LightSourceLine& source : m_scopeOne.configuration().lightSources)
        {
            if (!source.powerProperty.isEmpty() && loaded(source.powerDevice, source.powerProperty)
                && loaded(source.enableDevice, source.enableProperty)
                && loaded(source.readingDevice, source.readingProperty))
            {
                sources.push_back(source);
            }
        }
        return sources;
    }

    bool HardwareRuntime::setLightSource(const LightSourceLine& source, QString* errorMessage)
    {
        QString error;
        auto document = HardwareConfiguration::read(m_configurationPath, &error);
        if (!error.isEmpty()) { if (errorMessage) *errorMessage = error; return false; }
        document.setLightSource(source);
        if (!document.write(m_configurationPath, errorMessage)) return false;
        m_scopeOne.setLightSources(document.lightSources());
        return true;
    }

    // Takes the Core roles and the light sources of the cfg file again after an edit, without
    // restarting a device. The roles of Micro-Manager devices are set on its Core.
    bool HardwareRuntime::reloadRoles(QString* errorMessage)
    {
        QString error;
        const auto document = HardwareConfiguration::read(m_configurationPath, &error);
        if (!error.isEmpty()) { if (errorMessage) *errorMessage = error; return false; }
        applyRoles(document);
        m_scopeOne.setLightSources(document.lightSources());
        // A role that moved to a ScopeOne device is cleared in MMCore, which had kept driving the
        // earlier Micro-Manager device beside the new one
        const QStringList mmLabels = document.microManagerDeviceLabels();
        for (const QString& role : {QStringLiteral("Camera"), QStringLiteral("Shutter"), QStringLiteral("Focus"),
                                    QStringLiteral("XYStage"), QStringLiteral("AutoFocus")})
        {
            const QString value = document.coreRole(role);
            if (!m_microManager->setProperty(QStringLiteral("Core"), role,
                                             mmLabels.contains(value) ? value : QString{}, errorMessage)) return false;
        }
        applyCoreRoles();
        return true;
    }

    // The preset names of the group in cfg order, which are the Micro-Manager ones first
    QStringList HardwareRuntime::availableConfigs(const QString& groupName) const
    {
        const QString group = groupName.trimmed();
        QStringList presets = m_microManager->availableConfigs(group);
        for (const ScopeOneConfigGroupLine& line : scopeOneConfigLines())
        {
            if (line.group == group && !presets.contains(line.preset))
            {
                presets.append(line.preset);
            }
        }
        return presets;
    }

    // The groups of Micro-Manager, then the groups that only ScopeOne devices define
    QStringList HardwareRuntime::availableConfigGroups() const
    {
        QStringList groups = m_microManager->availableConfigGroups();
        for (const ScopeOneConfigGroupLine& line : scopeOneConfigLines())
        {
            if (!groups.contains(line.group))
            {
                groups.append(line.group);
            }
        }
        return groups;
    }

    // The Micro-Manager settings of the preset, then those of the ScopeOne devices
    std::vector<scopeone::core::ConfigSetting> HardwareRuntime::configSettings(
        const QString& groupName, const QString& configName) const
    {
        std::vector<scopeone::core::ConfigSetting> settings =
            m_microManager->configSettings(groupName.trimmed(), configName);
        // ScopeOne role lines are removed from the temporary MM cfg before loading. Keep them in
        // the editor's view so editing System/Startup writes the original role lines back.
        if (groupName.trimmed() == QStringLiteral("System")
            && configName == QStringLiteral("Startup") && !m_configurationPath.isEmpty())
        {
            QString error;
            const auto document = HardwareConfiguration::read(m_configurationPath, &error);
            if (error.isEmpty())
            {
                for (const auto& group : document.configGroups())
                    if (group.name == QStringLiteral("System"))
                        for (const auto& preset : group.presets)
                            if (preset.name == QStringLiteral("Startup"))
                                for (const auto& setting : preset.settings)
                                    if (setting.device == QStringLiteral("Core")
                                        && !std::any_of(settings.cbegin(), settings.cend(), [&](const auto& current)
                                        { return current.device == setting.device && current.property == setting.property; }))
                                        settings.push_back({setting.device, setting.property, setting.value});
            }
        }
        for (const ScopeOneConfigGroupLine& line : scopeOneConfigLines())
        {
            if (line.group == groupName.trimmed() && line.preset == configName)
            {
                settings.push_back({line.label, line.property, line.value});
            }
        }
        return settings;
    }

    // The first preset of the group that both parts match. The ScopeOne values come from the cache
    // of the DriverHost clients, so the refresh of the interface sends nothing to the devices.
    QString HardwareRuntime::currentConfig(const QString& groupName) const
    {
        const QString group = groupName.trimmed();
        const QString microManagerCurrent = m_microManager->currentConfig(group);
        const QStringList microManagerPresets = m_microManager->availableConfigs(group);
        const std::vector<ScopeOneConfigGroupLine> lines = scopeOneConfigLines();
        for (const QString& preset : availableConfigs(group))
        {
            // Presets that differ only in their ScopeOne part have the same Micro-Manager settings,
            // and Micro-Manager reports just one of them, so the settings are compared.
            if (microManagerPresets.contains(preset) && microManagerCurrent != preset
                && !sameSettings(m_microManager->configSettings(group, preset),
                                 m_microManager->configSettings(group, microManagerCurrent)))
            {
                continue;
            }
            bool matches = true;
            for (const ScopeOneConfigGroupLine& line : lines)
            {
                if (line.group != group || line.preset != preset)
                {
                    continue;
                }
                Device* device = propertyDeviceForDevice(line.label);
                if (!device || !propertyValuesEqual(device->getProperty(line.label, line.property, true),
                                                    line.value))
                {
                    matches = false;
                    break;
                }
            }
            if (matches)
            {
                return preset;
            }
        }
        return {};
    }

    // Sets the ScopeOne part of a preset, stopping at the first property that cannot be set
    bool HardwareRuntime::applyScopeOnePreset(const QString& groupName,
                                              const QString& configName,
                                              QString* errorMessage)
    {
        for (const ScopeOneConfigGroupLine& line : scopeOneConfigLines())
        {
            if (line.group != groupName.trimmed() || line.preset != configName)
            {
                continue;
            }
            QString error;
            if (!setProperty(line.label, line.property, line.value, &error))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("%1 %2: %3").arg(line.label, line.property, error);
                }
                return false;
            }
        }
        return true;
    }

    // Applies the Micro-Manager part of the preset, if it has one, then the ScopeOne part
    bool HardwareRuntime::setConfig(const QString& groupName,
                                    const QString& configName,
                                    QString* errorMessage)
    {
        // A preset that neither side defines is an error, so a misspelt name does not pass as set
        if (!availableConfigs(groupName).contains(configName))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("No preset %1 in group %2").arg(configName, groupName);
            }
            return false;
        }
        if (m_microManager->availableConfigs(groupName.trimmed()).contains(configName)
            && !m_microManager->setConfig(groupName.trimmed(), configName, errorMessage))
        {
            return false;
        }
        return applyScopeOnePreset(groupName, configName, errorMessage);
    }

    namespace
    {
        bool editError(QString* error, const QString& message)
        {
            if (error) *error = message;
            return false;
        }

        bool validName(const QString& name) { return !name.isEmpty() && HardwareConfiguration::isValidField(name); }

        bool hasGroup(const HardwareConfiguration& document, const QString& name)
        {
            for (const auto& group : document.configGroups())
                if (group.name == name) return true;
            return false;
        }

        bool hasPreset(const HardwareConfiguration& document, const QString& groupName, const QString& presetName)
        {
            for (const auto& group : document.configGroups())
                if (group.name == groupName)
                    for (const auto& preset : group.presets)
                        if (preset.name == presetName) return true;
            return false;
        }
    }

    // Snapshot the affected groups before changing either provider or the file.
    bool HardwareRuntime::editConfigGroups(
        const QStringList& groups,
        const std::function<bool(HardwareConfiguration&, QString*)>& edit, QString* error)
    {
        if (error) error->clear();
        if (m_configurationPath.isEmpty()) return editError(error, QStringLiteral("No configuration file is loaded"));
        QString failure;
        auto document = HardwareConfiguration::read(m_configurationPath, &failure);
        if (!failure.isEmpty()) return editError(error, failure);

        QList<HardwareConfiguration::Group> mmSnapshot;
        const QStringList mmGroups = m_microManager->availableConfigGroups();
        for (const auto& name : groups)
        {
            if (!mmGroups.contains(name)) continue;
            HardwareConfiguration::Group group{name, {}};
            for (const auto& presetName : m_microManager->availableConfigs(name))
            {
                HardwareConfiguration::Preset preset{presetName, {}};
                for (const auto& setting : m_microManager->configSettings(name, presetName))
                    preset.settings.append({setting, HardwareConfiguration::Source::MicroManager});
                group.presets.append(preset);
            }
            mmSnapshot.append(group);
        }
        QList<ScopeOneConfigGroupLine> scopeSnapshot;
        for (const auto& line : m_scopeOne.configuration().configGroups)
            if (groups.contains(line.group)) scopeSnapshot.append(line);

        if (!edit(document, &failure)) return editError(error, failure);
        const auto editedGroups = document.configGroups();
        for (const auto& group : editedGroups)
        {
            if (!groups.contains(group.name)) continue;
            if (!validName(group.name)) return editError(error, QStringLiteral("Invalid group name"));
            for (const auto& preset : group.presets)
            {
                if (!validName(preset.name) || preset.settings.isEmpty())
                    return editError(error, QStringLiteral("Preset needs a valid name and at least one property"));
                for (const auto& setting : preset.settings)
                    if (!validName(setting.device) || !validName(setting.property)
                        || !HardwareConfiguration::isValidField(setting.value, setting.source == HardwareConfiguration::Source::ScopeOne))
                        return editError(error, QStringLiteral("Setting of %1.%2 cannot be written to cfg")
                                         .arg(setting.device, setting.property));
            }
        }

        const auto clear = [&](QStringList& failures)
        {
            for (const auto& name : groups)
            {
                QString reason;
                if (m_microManager->availableConfigGroups().contains(name)
                    && !m_microManager->deleteConfigGroup(name, &reason)) failures.append(reason);
                reason.clear();
                if (!m_scopeOne.deleteConfigGroup(name, &reason)) failures.append(reason);
            }
        };
        QStringList errors;
        clear(errors);
        bool ok = errors.isEmpty();
        if (!ok) failure = errors.join(QStringLiteral("; "));
        for (const auto& group : editedGroups)
        {
            if (!ok || !groups.contains(group.name)) continue;
            ok = m_microManager->defineConfigGroup(group.name, &failure);
            for (const auto& preset : group.presets)
            {
                if (!ok) break;
                std::vector<ConfigSetting> mm;
                QList<ScopeOneConfigGroupLine> scope;
                for (const auto& setting : preset.settings)
                {
                    const bool scopeOneRole = group.name == QStringLiteral("System")
                        && preset.name == QStringLiteral("Startup")
                        && setting.device == QStringLiteral("Core")
                        && (setting.property == QStringLiteral("Camera")
                            || setting.property == QStringLiteral("Shutter")
                            || setting.property == QStringLiteral("Focus"))
                        && document.namesScopeOneDevice(setting.value);
                    if (scopeOneRole) continue;
                    if (setting.source == HardwareConfiguration::Source::MicroManager)
                        mm.push_back(setting);
                    else
                        scope.append({group.name, preset.name, setting.device, setting.property, setting.value});
                }
                if (!mm.empty()) ok = m_microManager->defineConfig(group.name, preset.name, mm, &failure);
                if (ok && !scope.isEmpty()) ok = m_scopeOne.defineConfig(group.name, preset.name, scope, &failure);
            }
        }
        if (ok) ok = document.write(m_configurationPath, &failure);
        if (ok) return true;

        QStringList recoveryErrors;
        clear(recoveryErrors);
        for (const auto& group : mmSnapshot)
        {
            QString reason;
            if (!m_microManager->defineConfigGroup(group.name, &reason)) recoveryErrors.append(reason);
            for (const auto& preset : group.presets)
            {
                std::vector<ConfigSetting> settings;
                for (const auto& setting : preset.settings) settings.push_back(setting);
                reason.clear();
                if (!m_microManager->defineConfig(group.name, preset.name, settings, &reason))
                    recoveryErrors.append(reason);
            }
        }
        QSet<QString> restored;
        for (const auto& line : scopeSnapshot)
        {
            const QString key = line.group + QChar(0x1f) + line.preset;
            if (restored.contains(key)) continue;
            restored.insert(key);
            QList<ScopeOneConfigGroupLine> settings;
            for (const auto& setting : scopeSnapshot)
                if (setting.group == line.group && setting.preset == line.preset) settings.append(setting);
            QString reason;
            if (!m_scopeOne.defineConfig(line.group, line.preset, settings, &reason)) recoveryErrors.append(reason);
        }
        if (!recoveryErrors.isEmpty())
            failure += QStringLiteral("; restoring configuration failed: %1").arg(recoveryErrors.join(QStringLiteral("; ")));
        return editError(error, failure);
    }

    bool HardwareRuntime::presetSettingsForEdit(
        const QList<ConfigSetting>& settings, QList<HardwareConfiguration::PresetSetting>& rows, QString* error) const
    {
        if (settings.isEmpty()) return editError(error, QStringLiteral("Select at least one property"));
        for (const auto& setting : settings)
        {
            const auto device = m_registry.device(setting.device);
            const bool coreDevice = setting.device == QStringLiteral("Core");
            if (!coreDevice && device.logicalId.isEmpty()) return editError(error, QStringLiteral("Unknown device %1").arg(setting.device));
            const auto source = coreDevice || device.providerId == QStringLiteral("micro-manager")
                ? HardwareConfiguration::Source::MicroManager : HardwareConfiguration::Source::ScopeOne;
            if (!validName(setting.device) || !validName(setting.property)
                || !HardwareConfiguration::isValidField(setting.value, source == HardwareConfiguration::Source::ScopeOne))
                return editError(error, QStringLiteral("Setting of %1.%2 cannot be written to cfg")
                                 .arg(setting.device, setting.property));
            for (const auto& row : rows)
                if (row.device == setting.device && row.property == setting.property)
                    return editError(error, QStringLiteral("Property selected twice: %1.%2")
                                     .arg(setting.device, setting.property));
            rows.append({setting, source});
        }
        return true;
    }

    bool HardwareRuntime::addConfigGroup(const QString& group, QString* error)
    {
        const QString name = group.trimmed();
        if (!validName(name)) return editError(error, QStringLiteral("Invalid group name"));
        return editConfigGroups({name}, [&](auto& document, QString* reason)
        {
            if (hasGroup(document, name)) return editError(reason, QStringLiteral("Group already exists: %1").arg(name));
            document.addConfigGroup(name);
            return true;
        }, error);
    }

    bool HardwareRuntime::removeConfigGroup(const QString& group, QString* error)
    {
        const QString name = group.trimmed();
        return editConfigGroups({name}, [&](auto& document, QString* reason)
        {
            if (!hasGroup(document, name)) return editError(reason, QStringLiteral("No group %1").arg(name));
            document.removeConfigGroup(name);
            return true;
        }, error);
    }

    bool HardwareRuntime::renameConfigGroup(const QString& group, const QString& newGroup, QString* error)
    {
        const QString name = group.trimmed(), next = newGroup.trimmed();
        if (!validName(next)) return editError(error, QStringLiteral("Invalid group name"));
        return editConfigGroups({name, next}, [&](auto& document, QString* reason)
        {
            if (!hasGroup(document, name)) return editError(reason, QStringLiteral("No group %1").arg(name));
            if (hasGroup(document, next)) return editError(reason, QStringLiteral("Group already exists: %1").arg(next));
            document.renameConfigGroup(name, next);
            return true;
        }, error);
    }

    bool HardwareRuntime::setConfigPreset(const QString& group, const QString& preset,
                                         const std::vector<ConfigSetting>& settings, QString* error)
    {
        const QString name = group.trimmed(), presetName = preset.trimmed();
        if (!validName(presetName)) return editError(error, QStringLiteral("Invalid preset name"));
        QList<HardwareConfiguration::PresetSetting> rows;
        const QList<ConfigSetting> values(settings.cbegin(), settings.cend());
        if (!presetSettingsForEdit(values, rows, error)) return false;
        return editConfigGroups({name}, [&](auto& document, QString* reason)
        {
            if (!hasGroup(document, name)) return editError(reason, QStringLiteral("No group %1").arg(name));
            document.setPreset(name, presetName, rows);
            return true;
        }, error);
    }

    bool HardwareRuntime::removeConfigPreset(const QString& group, const QString& preset, QString* error)
    {
        const QString name = group.trimmed(), presetName = preset.trimmed();
        return editConfigGroups({name}, [&](auto& document, QString* reason)
        {
            if (!hasPreset(document, name, presetName))
                return editError(reason, QStringLiteral("No preset %1 in group %2").arg(presetName, name));
            document.removePreset(name, presetName);
            if (!hasGroup(document, name)) document.addConfigGroup(name);
            return true;
        }, error);
    }

    bool HardwareRuntime::renameConfigPreset(const QString& group, const QString& preset,
                                            const QString& newPreset, QString* error)
    {
        const QString name = group.trimmed(), old = preset.trimmed(), next = newPreset.trimmed();
        if (!validName(next)) return editError(error, QStringLiteral("Invalid preset name"));
        return editConfigGroups({name}, [&](auto& document, QString* reason)
        {
            if (!hasPreset(document, name, old))
                return editError(reason, QStringLiteral("No preset %1 in group %2").arg(old, name));
            if (hasPreset(document, name, next))
                return editError(reason, QStringLiteral("Preset already exists: %1").arg(next));
            document.renamePreset(name, old, next);
            return true;
        }, error);
    }

    bool HardwareRuntime::setConfigGroupProperties(const QString& group, const QList<ConfigSetting>& properties,
                                                  QString* error)
    {
        const QString name = group.trimmed();
        QList<HardwareConfiguration::PresetSetting> selected;
        if (!presetSettingsForEdit(properties, selected, error)) return false;
        return editConfigGroups({name}, [&](auto& document, QString* reason)
        {
            for (const auto& current : document.configGroups())
            {
                if (current.name != name) continue;
                if (current.presets.isEmpty()) document.setPreset(name, QStringLiteral("Default"), selected);
                for (const auto& preset : current.presets)
                {
                    auto rows = selected;
                    for (auto& row : rows)
                        for (const auto& previous : preset.settings)
                            if (row.device == previous.device && row.property == previous.property)
                            {
                                row.value = previous.value;
                                break;
                            }
                    document.setPreset(name, preset.name, rows);
                }
                return true;
            }
            return editError(reason, QStringLiteral("No group %1").arg(name));
        }, error);
    }

    // System/Startup of the ScopeOne devices, after the load. MMCore has applied its own part.
    void HardwareRuntime::applyScopeOneStartupPreset()
    {
        QString error;
        if (!applyScopeOnePreset(QStringLiteral("System"), QStringLiteral("Startup"), &error))
        {
            qWarning().noquote() << QStringLiteral("System/Startup failed: %1").arg(error);
        }
    }

    // MMCore loads the cfg without the role lines that name ScopeOne devices and gives those
    // roles to a Micro-Manager device it loads, which then took the role over from the ScopeOne
    // device. They are cleared here. The shutter of a ScopeOne device was asked to open around
    // the images by itself, which it cannot, as the cameras run in other DriverHosts.
    void HardwareRuntime::applyCoreRoles()
    {
        QString error;
        if (!m_microManager->setProperty(QStringLiteral("Core"), QStringLiteral("AutoShutter"), QStringLiteral("0"), &error))
            qWarning().noquote() << QStringLiteral("AutoShutter failed: %1").arg(error);
        const auto document = HardwareConfiguration::read(m_configurationPath, nullptr);
        const std::pair<QString, QString*> roles[]{{QStringLiteral("Shutter"), &m_shutterRole},
                                                   {QStringLiteral("Focus"), &m_focusRole},
                                                   {QStringLiteral("XYStage"), &m_xyStageRole},
                                                   {QStringLiteral("AutoFocus"), &m_autofocusRole}};
        for (const auto& [role, value] : roles)
        {
            if (document.namesScopeOneDevice(*value))
                m_microManager->setProperty(QStringLiteral("Core"), role, QString{}, nullptr);
        }
    }

    bool HardwareRuntime::anyCameraRunning() const
    {
        const QList<HardwareDeviceDescriptor> devices = m_registry.devices();
        return std::any_of(devices.cbegin(), devices.cend(), [this](const HardwareDeviceDescriptor& device)
        {
            return device.kind == HardwareDeviceKind::Camera && isPreviewRunning(device.logicalId);
        });
    }

    // The device of a Core role. MMCore holds a Micro-Manager device, which presets may change, as
    // a channel that switches the shutter. A ScopeOne device comes from the cfg. Otherwise the
    // first device of the kind takes the role, as MMCore gives a role to a device it loads.
    QString HardwareRuntime::roleDevice(const QString& role, const QString& configured, HardwareDeviceKind kind) const
    {
        QString id = m_microManager->getProperty(QStringLiteral("Core"), role, false);
        if (id.isEmpty()) id = configured;
        if (id.isEmpty())
        {
            for (const HardwareDeviceDescriptor& device : m_registry.devices())
                if (device.kind == kind) return device.logicalId;
        }
        return m_registry.device(id).kind == kind ? id : QString{};
    }

    bool HardwareRuntime::setDefaultShutterOpen(bool open, QString* errorMessage)
    {
        QString shutter;
        return forward(configuredShutter(&shutter), "No shutter lights the cameras", errorMessage,
                       [&](Shutter& provider) { return provider.setShutterOpen(shutter, open, errorMessage); });
    }

    QString HardwareRuntime::defaultShutter() const
    {
        QString shutter;
        configuredShutter(&shutter);
        return shutter;
    }

    Shutter* HardwareRuntime::configuredShutter(QString* deviceId) const
    {
        *deviceId = roleDevice(QStringLiteral("Shutter"), m_shutterRole, HardwareDeviceKind::Shutter);
        return deviceId->isEmpty() ? nullptr : shutterForDevice(*deviceId);
    }

    Autofocus* HardwareRuntime::configuredAutofocus(QString* deviceId) const
    {
        *deviceId = roleDevice(QStringLiteral("AutoFocus"), m_autofocusRole, HardwareDeviceKind::AutoFocus);
        return deviceId->isEmpty() ? nullptr : dynamic_cast<Autofocus*>(m_registry.providerForDevice(*deviceId).get());
    }

    void HardwareRuntime::setConfigurationPath(const QString& path)
    {
        m_configurationPath = path;
        applyRoles(path.isEmpty() ? HardwareConfiguration{} : HardwareConfiguration::read(path, nullptr));
    }

    void HardwareRuntime::applyRoles(const HardwareConfiguration& document)
    {
        m_cameraRole = document.coreRole(QStringLiteral("Camera"));
        m_xyStageRole = document.coreRole(QStringLiteral("XYStage"));
        m_focusRole = document.coreRole(QStringLiteral("Focus"));
        m_shutterRole = document.coreRole(QStringLiteral("Shutter"));
        m_autofocusRole = document.coreRole(QStringLiteral("AutoFocus"));
        m_autoShutter = document.coreRole(QStringLiteral("AutoShutter")) != QStringLiteral("0");
    }

    bool HardwareRuntime::hasAutofocus() const
    {
        QString device;
        return configuredAutofocus(&device) != nullptr;
    }

    bool HardwareRuntime::runAutofocus(QString* errorMessage)
    {
        QString device;
        return forward(configuredAutofocus(&device), "Autofocus is not available", errorMessage,
                       [&](Autofocus& provider) { return provider.runAutofocus(device, errorMessage); });
    }

    long HardwareRuntime::focusSequenceCapacity(const QString& focusDevice) const
    {
        ZStage* provider = zStageForDevice(focusDevice);
        return provider ? provider->stageSequenceCapacity(focusDevice) : 0;
    }

    bool HardwareRuntime::startFocusSequence(const QString& focusDevice,
                                             const std::vector<double>& positions,
                                             QString* errorMessage)
    {
        return forward(zStageForDevice(focusDevice), "Z stage provider not available", errorMessage,
                       [&](ZStage& provider)
                       {
                           return provider.startStageSequence(focusDevice, positions, errorMessage);
                       });
    }

    void HardwareRuntime::stopFocusSequence(const QString& focusDevice)
    {
        if (ZStage* provider = zStageForDevice(focusDevice))
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
        if (m_registry.device(cameraId).providerId == m_microManager->descriptor().id)
        {
            return m_microManager->pixelSizeUm(cameraId);
        }
        // The camera of a driver takes the pixel size calibration of the cfg as well, which is
        // scaled by its binning, as in Micro-Manager. Cameras name their binning "2" or "2x2".
        Device* const device = propertyDeviceForDevice(cameraId);
        const int binning = device ? device->getProperty(cameraId, QStringLiteral("Binning"), true)
                                         .section(QLatin1Char('x'), 0, 0).toInt()
                                   : 0;
        return m_microManager->calibratedPixelSizeUm() * (std::max)(binning, 1);
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

    ScopeOneProvider::LoadResult HardwareRuntime::loadScopeOneDevices(const QString& configPath)
    {
        return m_scopeOne.load(configPath);
    }

    bool HardwareRuntime::registerScopeOneDevices()
    {
        for (const DevicePtr& device : m_scopeOne.devices())
        {
            if (!registerProvider(device))
            {
                return false;
            }
        }
        return true;
    }

    QList<DevicePtr> HardwareRuntime::takeDriverHosts()
    {
        QList<DevicePtr> hosts = m_microManager->takeCameras();
        for (const DevicePtr& device : m_scopeOne.takeDevices())
        {
            unregisterProvider(device->descriptor().id);
            hosts.append(device);
        }
        return hosts;
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

    // System/Shutdown of the ScopeOne devices, which the caller runs off the GUI thread
    void HardwareRuntime::applyScopeOneShutdownPreset()
    {
        QString error;
        if (!applyScopeOnePreset(QStringLiteral("System"), QStringLiteral("Shutdown"), &error))
        {
            qWarning().noquote() << QStringLiteral("System/Shutdown failed: %1").arg(error);
        }
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
        auto* xyStageProvider = dynamic_cast<XYStage*>(provider.get());
        auto* zStageProvider = dynamic_cast<ZStage*>(provider.get());
        auto* shutterProvider = dynamic_cast<Shutter*>(provider.get());
        auto* stateProvider = dynamic_cast<StateDevice*>(provider.get());
        const QList<HardwareDeviceDescriptor> providerDevices = provider->devices();
        for (const HardwareDeviceDescriptor& device : providerDevices)
        {
            const bool supported =
                (device.kind != HardwareDeviceKind::Camera || cameraProvider)
                && (device.kind != HardwareDeviceKind::XYStage || xyStageProvider)
                && (device.kind != HardwareDeviceKind::ZStage || zStageProvider)
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
                    const bool running = anyCameraRunning();
                    // Also a camera that stopped by itself closes the shutter
                    if (!running && m_autoShutter) setDefaultShutterOpen(false);
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

    void HardwareRuntime::stopPreviewForProvider(const QString& providerId)
    {
        stopCameras(m_registry, providerId);
    }
}

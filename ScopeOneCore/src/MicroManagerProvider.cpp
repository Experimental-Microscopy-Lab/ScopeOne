#include "internal/MicroManagerProvider.h"

#include "internal/DriverHostClient.h"

#include "internal/MicroManagerCalls.h"

#include <QDebug>
#include <QFuture>
#include <QJsonDocument>
#include <QtConcurrent/QtConcurrentRun>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <utility>
#include <vector>

namespace scopeone::core::internal
{
    using namespace micromanager;

    MicroManagerProvider::MicroManagerProvider()
        : m_core(std::make_shared<CMMCore>())
    {
    }

    MicroManagerProvider::~MicroManagerProvider() = default;

    QString MicroManagerProvider::coreVersion()
    {
        return QStringLiteral("%1.%2.%3")
            .arg(CMMCore::getMMCoreVersionMajor())
            .arg(CMMCore::getMMCoreVersionMinor())
            .arg(CMMCore::getMMCoreVersionPatch());
    }

    HardwareProviderDescriptor MicroManagerProvider::descriptor() const
    {
        return micromanager::descriptor();
    }

    QList<HardwareDeviceDescriptor> MicroManagerProvider::devices() const
    {
        return m_devices;
    }

    void MicroManagerProvider::setDevices(const QList<HardwareDeviceDescriptor>& devices)
    {
        m_devices = devices;
    }

    QStringList MicroManagerProvider::startCameras(const QList<MicroManagerCameraSetup>& cameras)
    {
        if (cameras.size() == 1)
        {
            return startLocalCamera(cameras.first().cameraId) ? QStringList{} : QStringList{cameras.first().cameraId};
        }
        // Each DriverHost loads its camera on its own, which can take seconds, so they start
        // together
        QList<QFuture<DevicePtr>> hosts;
        for (const MicroManagerCameraSetup& setup : cameras)
        {
            hosts.append(QtConcurrent::run([setup]()
            {
                QString error;
                const QStringList arguments{
                    QStringLiteral("--micromanagerCamera"),
                    QString::fromUtf8(QJsonDocument(setup.toJson()).toJson(QJsonDocument::Compact))};
                DevicePtr host = startDriverHost(micromanager::descriptor().id, arguments, &error);
                if (!host)
                {
                    qWarning().noquote() << QStringLiteral("Failed to start the DriverHost of camera '%1': %2")
                                                .arg(setup.cameraId, error);
                }
                return host;
            }));
        }
        QStringList failed;
        for (qsizetype index = 0; index < cameras.size(); ++index)
        {
            const DevicePtr host = hosts[index].result();
            if (!host)
            {
                failed.append(cameras[index].cameraId);
                continue;
            }
            CameraEntry entry;
            entry.provider = host;
            entry.camera = dynamic_cast<Camera*>(host.get());
            entry.frameSource = dynamic_cast<CameraFrameSource*>(host.get());
            entry.camera->setPreviewStateSink(m_previewStateSink);
            entry.frameSource->setFrameDelivery(m_delivery);
            {
                QWriteLocker locker(&m_camerasLock);
                m_cameras.insert(cameras[index].cameraId, entry);
            }
            readBinning(cameras[index].cameraId);
        }
        return failed;
    }

    bool MicroManagerProvider::startLocalCamera(const QString& cameraId)
    {
        auto camera = std::make_shared<MicroManagerCamera>(m_core, cameraId, false);
        auto lastFrameIndex = std::make_shared<std::atomic<quint64>>(0);
        camera->setFrameSink([this, lastFrameIndex](const ImageFrame& frame)
        {
            CameraFrameDelivery* const delivery = m_delivery;
            if (!delivery)
            {
                return;
            }
            // The camera skips frames outside AllFrames mode, and their indices tell how many
            const quint64 previous = lastFrameIndex->exchange(frame.frameIndex);
            const quint64 acquired = previous == 0 || frame.frameIndex <= previous ? 1 : frame.frameIndex - previous;
            delivery->deliverFrames({frame}, acquired);
        });
        camera->setFrameFilter([this, cameraId]()
        {
            CameraFrameDelivery* const delivery = m_delivery;
            return !delivery || delivery->wantsLatestFrame(cameraId);
        });
        camera->setErrorSink([this](const QString& error)
        {
            if (CameraFrameDelivery* const delivery = m_delivery)
            {
                delivery->fail(error);
            }
        });
        camera->setPreviewStateSink(m_previewStateSink);
        CameraEntry entry;
        entry.provider = camera;
        entry.camera = camera.get();
        entry.local = camera;
        entry.lastFrameIndex = lastFrameIndex;
        QWriteLocker locker(&m_camerasLock);
        m_cameras.insert(cameraId, entry);
        return true;
    }

    QList<DevicePtr> MicroManagerProvider::takeCameras()
    {
        QMap<QString, CameraEntry> taken;
        {
            QWriteLocker locker(&m_camerasLock);
            taken.swap(m_cameras);
            m_binnings.clear();
        }
        QList<DevicePtr> providers;
        for (const CameraEntry& entry : std::as_const(taken))
        {
            entry.camera->setPreviewStateSink({});
            providers.append(entry.provider);
        }
        return providers;
    }

    MicroManagerProvider::CameraRef MicroManagerProvider::camera(const QString& cameraId) const
    {
        const CameraEntry entry = cameraEntry(cameraId);
        return {entry.provider, entry.camera};
    }

    MicroManagerProvider::CameraEntry MicroManagerProvider::cameraEntry(const QString& cameraId) const
    {
        QReadLocker locker(&m_camerasLock);
        return m_cameras.value(cameraId.trimmed());
    }

    QList<MicroManagerProvider::CameraEntry> MicroManagerProvider::cameras() const
    {
        QReadLocker locker(&m_camerasLock);
        return m_cameras.values();
    }

    void MicroManagerProvider::setFrameDelivery(CameraFrameDelivery* delivery)
    {
        m_delivery = delivery;
        for (const CameraEntry& entry : cameras())
        {
            if (entry.frameSource)
            {
                entry.frameSource->setFrameDelivery(delivery);
            }
        }
    }

    bool MicroManagerProvider::setFrameDeliveryMode(const QStringList& cameraIds, FrameDeliveryMode mode)
    {
        for (const QString& cameraId : cameraIds)
        {
            const CameraEntry entry = cameraEntry(cameraId);
            if (entry.local)
            {
                entry.local->setFrameDeliveryMode(mode);
                entry.lastFrameIndex->store(0);
            }
            else if (entry.frameSource && !entry.frameSource->setFrameDeliveryMode({cameraId}, mode))
            {
                return false;
            }
        }
        return true;
    }

    void MicroManagerProvider::setFrameDeliveryPaused(const QStringList& cameraIds, bool paused)
    {
        for (const QString& cameraId : cameraIds)
        {
            const CameraEntry entry = cameraEntry(cameraId);
            if (entry.local)
            {
                entry.local->setFrameDeliveryPaused(paused);
            }
            else if (entry.frameSource)
            {
                entry.frameSource->setFrameDeliveryPaused({cameraId}, paused);
            }
        }
    }

    // The frames go to the CameraFrameDelivery instead
    void MicroManagerProvider::setFrameSink(FrameSink)
    {
    }

    void MicroManagerProvider::setPreviewStateSink(PreviewStateSink sink)
    {
        m_previewStateSink = std::move(sink);
        for (const CameraEntry& entry : cameras())
        {
            entry.camera->setPreviewStateSink(m_previewStateSink);
        }
    }

    bool MicroManagerProvider::startPreviewFor(const QString& cameraId)
    {
        const CameraRef provider = camera(cameraId);
        return provider && provider->startPreviewFor(cameraId.trimmed());
    }

    bool MicroManagerProvider::stopPreviewFor(const QString& cameraId)
    {
        const CameraRef provider = camera(cameraId);
        return provider && provider->stopPreviewFor(cameraId.trimmed());
    }

    bool MicroManagerProvider::isPreviewRunning(const QString& cameraId) const
    {
        const CameraRef provider = camera(cameraId);
        return provider && provider->isPreviewRunning(cameraId.trimmed());
    }

    bool MicroManagerProvider::getExposure(const QString& cameraId, double& exposureMs) const
    {
        const CameraRef provider = camera(cameraId);
        return provider && provider->getExposure(cameraId.trimmed(), exposureMs);
    }

    bool MicroManagerProvider::setExposure(const QString& cameraId, double exposureMs)
    {
        const CameraRef provider = camera(cameraId);
        return provider && provider->setExposure(cameraId.trimmed(), exposureMs);
    }

    QStringList MicroManagerProvider::listProperties(const QString& cameraId)
    {
        if (const CameraRef provider = camera(cameraId))
        {
            return provider.owner->listProperties(cameraId.trimmed());
        }
        return orElse(QStringList{}, [&]
        {
            return toQStringList(m_core->getDevicePropertyNames(name8(cameraId).c_str()));
        });
    }

    QString MicroManagerProvider::getProperty(const QString& cameraId,
                                              const QString& name,
                                              bool fromCache)
    {
        if (const CameraRef provider = camera(cameraId))
        {
            return provider.owner->getProperty(cameraId.trimmed(), name, fromCache);
        }
        return orElse(QString{}, [&]
        {
            const std::string device = name8(cameraId);
            const std::string property = name8(name);
            return QString::fromStdString(
                fromCache
                    ? m_core->getPropertyFromCache(device.c_str(), property.c_str())
                    : m_core->getProperty(device.c_str(), property.c_str()));
        });
    }

    bool MicroManagerProvider::setProperty(const QString& cameraId,
                                           const QString& name,
                                           const QString& value,
                                           QString* errorMessage)
    {
        if (const CameraRef provider = camera(cameraId))
        {
            const bool ok = provider.owner->setProperty(cameraId.trimmed(), name, value, errorMessage);
            if (ok && name.trimmed() == QLatin1String(MM::g_Keyword_Binning))
            {
                readBinning(cameraId.trimmed());
            }
            return ok;
        }
        return succeeds(errorMessage, [&]
        {
            const std::string device = name8(cameraId);
            m_core->setProperty(device.c_str(), name8(name).c_str(), value.toStdString().c_str());
            m_core->waitForDevice(device.c_str());
            m_core->updateSystemStateCache();
        });
    }

    QString MicroManagerProvider::getPropertyType(const QString& cameraId, const QString& name)
    {
        if (const CameraRef provider = camera(cameraId))
        {
            return provider.owner->getPropertyType(cameraId.trimmed(), name);
        }
        return micromanager::propertyType(*m_core, cameraId, name);
    }

    bool MicroManagerProvider::isPropertyReadOnly(const QString& cameraId, const QString& name)
    {
        if (const CameraRef provider = camera(cameraId))
        {
            return provider.owner->isPropertyReadOnly(cameraId.trimmed(), name);
        }
        return orElse(true, [&]
        {
            return m_core->isPropertyReadOnly(name8(cameraId).c_str(), name8(name).c_str());
        });
    }

    bool MicroManagerProvider::isPropertyPreInit(const QString& cameraId, const QString& name)
    {
        if (const CameraRef provider = camera(cameraId))
        {
            return provider.owner->isPropertyPreInit(cameraId.trimmed(), name);
        }
        return orElse(false, [&]
        {
            return m_core->isPropertyPreInit(name8(cameraId).c_str(), name8(name).c_str());
        });
    }

    QStringList MicroManagerProvider::getAllowedPropertyValues(const QString& cameraId,
                                                               const QString& name)
    {
        if (const CameraRef provider = camera(cameraId))
        {
            return provider.owner->getAllowedPropertyValues(cameraId.trimmed(), name);
        }
        return orElse(QStringList{}, [&]
        {
            return toQStringList(
                m_core->getAllowedPropertyValues(name8(cameraId).c_str(), name8(name).c_str()));
        });
    }

    bool MicroManagerProvider::hasPropertyLimits(const QString& cameraId, const QString& name)
    {
        if (const CameraRef provider = camera(cameraId))
        {
            return provider.owner->hasPropertyLimits(cameraId.trimmed(), name);
        }
        return orElse(false, [&]
        {
            return m_core->hasPropertyLimits(name8(cameraId).c_str(), name8(name).c_str());
        });
    }

    double MicroManagerProvider::getPropertyLowerLimit(const QString& cameraId, const QString& name)
    {
        if (const CameraRef provider = camera(cameraId))
        {
            return provider.owner->getPropertyLowerLimit(cameraId.trimmed(), name);
        }
        return orElse(0.0, [&]
        {
            return m_core->getPropertyLowerLimit(name8(cameraId).c_str(), name8(name).c_str());
        });
    }

    double MicroManagerProvider::getPropertyUpperLimit(const QString& cameraId, const QString& name)
    {
        if (const CameraRef provider = camera(cameraId))
        {
            return provider.owner->getPropertyUpperLimit(cameraId.trimmed(), name);
        }
        return orElse(0.0, [&]
        {
            return m_core->getPropertyUpperLimit(name8(cameraId).c_str(), name8(name).c_str());
        });
    }

    bool MicroManagerProvider::setROI(const QString& cameraId,
                                      int x,
                                      int y,
                                      int width,
                                      int height)
    {
        const CameraRef provider = camera(cameraId);
        return provider && provider->setROI(cameraId.trimmed(), x, y, width, height);
    }

    bool MicroManagerProvider::clearROI(const QString& cameraId)
    {
        const CameraRef provider = camera(cameraId);
        return provider && provider->clearROI(cameraId.trimmed());
    }

    bool MicroManagerProvider::getROI(const QString& cameraId,
                                      int& x,
                                      int& y,
                                      int& width,
                                      int& height)
    {
        const CameraRef provider = camera(cameraId);
        return provider && provider->getROI(cameraId.trimmed(), x, y, width, height);
    }

    bool MicroManagerProvider::captureEventFrame(const QString& cameraId,
                                                 scopeone::core::ImageFrame& frame,
                                                 int timeoutMs)
    {
        const CameraRef provider = camera(cameraId);
        return provider && provider->captureEventFrame(cameraId.trimmed(), frame, timeoutMs);
    }

    bool MicroManagerProvider::supportsEventSequence(const QString& cameraId) const
    {
        const CameraRef provider = camera(cameraId);
        return provider && provider->supportsEventSequence(cameraId.trimmed());
    }

    bool MicroManagerProvider::captureEventSequence(const QString& cameraId,
                                                    int count,
                                                    std::vector<scopeone::core::ImageFrame>& frames,
                                                    int timeoutMs)
    {
        const CameraRef provider = camera(cameraId);
        return provider && provider->captureEventSequence(cameraId.trimmed(), count, frames, timeoutMs);
    }

    QString MicroManagerProvider::defaultXYStage() const
    {
        return orElse(QString{}, [&] { return QString::fromStdString(m_core->getXYStageDevice()); });
    }

    QString MicroManagerProvider::defaultZStage() const
    {
        return orElse(QString{}, [&] { return QString::fromStdString(m_core->getFocusDevice()); });
    }

    bool MicroManagerProvider::getXYPosition(const QString& deviceId,
                                             double& x,
                                             double& y,
                                             QString* errorMessage) const
    {
        x = 0.0;
        y = 0.0;
        return succeeds(errorMessage, [&] { m_core->getXYPosition(name8(deviceId).c_str(), x, y); });
    }

    bool MicroManagerProvider::getZPosition(const QString& deviceId,
                                            double& z,
                                            QString* errorMessage) const
    {
        z = 0.0;
        return succeeds(errorMessage, [&] { z = m_core->getPosition(name8(deviceId).c_str()); });
    }

    bool MicroManagerProvider::setRelativeXYPosition(const QString& deviceId,
                                                     double dx,
                                                     double dy,
                                                     QString* errorMessage)
    {
        return succeeds(errorMessage, [&]
        {
            const std::string device = name8(deviceId);
            m_core->setRelativeXYPosition(device.c_str(), dx, dy);
            m_core->waitForDevice(device.c_str());
        });
    }

    bool MicroManagerProvider::setRelativeZPosition(const QString& deviceId,
                                                    double dz,
                                                    QString* errorMessage)
    {
        return succeeds(errorMessage, [&]
        {
            const std::string device = name8(deviceId);
            m_core->setRelativePosition(device.c_str(), dz);
            m_core->waitForDevice(device.c_str());
        });
    }

    bool MicroManagerProvider::setXYPosition(const QString& deviceId,
                                             double x,
                                             double y,
                                             QString* errorMessage)
    {
        return succeeds(errorMessage, [&]
        {
            const std::string device = name8(deviceId);
            m_core->setXYPosition(device.c_str(), x, y);
            m_core->waitForDevice(device.c_str());
        });
    }

    bool MicroManagerProvider::setZPosition(const QString& deviceId,
                                            double z,
                                            QString* errorMessage)
    {
        return succeeds(errorMessage, [&]
        {
            const std::string device = name8(deviceId);
            m_core->setPosition(device.c_str(), z);
            m_core->waitForDevice(device.c_str());
        });
    }

    bool MicroManagerProvider::isShutterOpen(const QString& deviceId,
                                             bool& open,
                                             QString* errorMessage) const
    {
        open = false;
        return succeeds(errorMessage, [&] { open = m_core->getShutterOpen(name8(deviceId).c_str()); });
    }

    bool MicroManagerProvider::setShutterOpen(const QString& deviceId,
                                              bool open,
                                              QString* errorMessage)
    {
        return succeeds(errorMessage, [&]
        {
            const std::string device = name8(deviceId);
            m_core->setShutterOpen(device.c_str(), open);
            m_core->waitForDevice(device.c_str());
            m_core->updateSystemStateCache();
        });
    }

    bool MicroManagerProvider::getState(const QString& deviceId,
                                        long& state,
                                        QString* errorMessage) const
    {
        state = 0;
        return succeeds(errorMessage, [&] { state = m_core->getState(name8(deviceId).c_str()); });
    }

    bool MicroManagerProvider::setState(const QString& deviceId,
                                        long state,
                                        QString* errorMessage)
    {
        return succeeds(errorMessage, [&]
        {
            const std::string device = name8(deviceId);
            m_core->setState(device.c_str(), state);
            m_core->waitForDevice(device.c_str());
            m_core->updateSystemStateCache();
        });
    }

    QString MicroManagerProvider::stateLabel(const QString& deviceId, long state) const
    {
        if (state < 0)
        {
            return {};
        }
        return orElse(QString{}, [&]
        {
            const std::vector<std::string> labels = m_core->getStateLabels(name8(deviceId).c_str());
            const size_t index = static_cast<size_t>(state);
            return index < labels.size() ? QString::fromStdString(labels[index]) : QString{};
        });
    }

    QStringList MicroManagerProvider::availableConfigGroups() const
    {
        return orElse(QStringList{}, [&] { return toQStringList(m_core->getAvailableConfigGroups()); });
    }

    QStringList MicroManagerProvider::availableConfigs(const QString& groupName) const
    {
        return orElse(QStringList{}, [&]
        {
            return toQStringList(m_core->getAvailableConfigs(name8(groupName).c_str()));
        });
    }

    std::vector<scopeone::core::ConfigSetting> MicroManagerProvider::configSettings(
        const QString& groupName, const QString& configName) const
    {
        return orElse(std::vector<scopeone::core::ConfigSetting>{}, [&]
        {
            std::vector<scopeone::core::ConfigSetting> settings;
            const Configuration preset = m_core->getConfigData(name8(groupName).c_str(),
                                                               name8(configName).c_str());
            for (size_t index = 0; index < preset.size(); ++index)
            {
                const PropertySetting setting = preset.getSetting(index);
                settings.push_back({QString::fromStdString(setting.getDeviceLabel()),
                                    QString::fromStdString(setting.getPropertyName()),
                                    QString::fromStdString(setting.getPropertyValue())});
            }
            return settings;
        });
    }

    QString MicroManagerProvider::currentConfig(const QString& groupName) const
    {
        return orElse(QString{}, [&]() -> QString
        {
            const std::string group = name8(groupName);
            const std::vector<std::string> configs = m_core->getAvailableConfigs(group.c_str());
            QHash<QString, QString> currentValues;
            QSet<QString> failedProperties;
            for (const std::string& config : configs)
            {
                const Configuration preset = m_core->getConfigData(group.c_str(), config.c_str());
                bool matches = true;
                for (size_t index = 0; index < preset.size(); ++index)
                {
                    const PropertySetting setting = preset.getSetting(index);
                    const QString device = QString::fromStdString(setting.getDeviceLabel());
                    const QString property = QString::fromStdString(setting.getPropertyName());
                    const QString key = device + QChar(0x1f) + property;
                    if (!currentValues.contains(key) && !failedProperties.contains(key))
                    {
                        const QString value = const_cast<MicroManagerProvider*>(this)->getProperty(
                            device, property, false);
                        if (value.isNull())
                        {
                            failedProperties.insert(key);
                        }
                        else
                        {
                            currentValues.insert(key, value);
                        }
                    }
                    if (failedProperties.contains(key)
                        || currentValues.value(key)
                               != QString::fromStdString(setting.getPropertyValue()))
                    {
                        matches = false;
                        break;
                    }
                }
                if (matches)
                {
                    return QString::fromStdString(config);
                }
            }
            return {};
        });
    }

    bool MicroManagerProvider::setConfig(const QString& groupName,
                                         const QString& configName,
                                         QString* errorMessage)
    {
        QString failure;
        const bool ok = succeeds(errorMessage, [&]
        {
            const Configuration preset = m_core->getConfigData(name8(groupName).c_str(),
                                                               name8(configName).c_str());
            std::vector<PropertySetting> pending;
            pending.reserve(preset.size());
            for (size_t index = 0; index < preset.size(); ++index)
            {
                pending.push_back(preset.getSetting(index));
            }

            // A setting can depend on another one in the same preset, so retry until a pass makes no progress
            while (!pending.empty())
            {
                std::vector<PropertySetting> failed;
                QString failureDescription;
                for (const PropertySetting& setting : pending)
                {
                    const QString device = QString::fromStdString(setting.getDeviceLabel());
                    const QString property = QString::fromStdString(setting.getPropertyName());
                    const QString value = QString::fromStdString(setting.getPropertyValue());
                    QString error;
                    if (!setProperty(device, property, value, &error))
                    {
                        failed.push_back(setting);
                        failureDescription = QStringLiteral("%1.%2 = %3: %4")
                                                 .arg(device, property, value, error);
                    }
                }
                if (failed.size() == pending.size())
                {
                    failure = QStringLiteral("Failed to apply config preset %1 = %2 at %3")
                                  .arg(groupName, configName, failureDescription);
                    return;
                }
                pending = std::move(failed);
            }

            m_core->waitForSystem();
            m_core->updateSystemStateCache();
        });
        if (ok && !failure.isEmpty())
        {
            if (errorMessage)
            {
                *errorMessage = failure;
            }
            return false;
        }
        return ok;
    }

    QString MicroManagerProvider::defaultShutter() const
    {
        return orElse(QString{}, [&] { return QString::fromStdString(m_core->getShutterDevice()); });
    }

    bool MicroManagerProvider::autoShutter() const
    {
        return orElse(false, [&] { return m_core->getAutoShutter(); });
    }

    bool MicroManagerProvider::setAutoShutter(bool enabled, QString* errorMessage)
    {
        return succeeds(errorMessage, [&] { m_core->setAutoShutter(enabled); });
    }

    QString MicroManagerProvider::defaultAutofocus() const
    {
        return orElse(QString{}, [&] { return QString::fromStdString(m_core->getAutoFocusDevice()); });
    }

    // The core focuses with the autofocus device it currently uses
    bool MicroManagerProvider::runAutofocus(const QString& deviceId, QString* errorMessage)
    {
        return succeeds(errorMessage, [&]
        {
            const std::string device = name8(deviceId);
            if (m_core->getAutoFocusDevice() != device)
            {
                m_core->setAutoFocusDevice(device.c_str());
            }
            m_core->fullFocus();
        });
    }

    long MicroManagerProvider::stageSequenceCapacity(const QString& deviceId) const
    {
        return orElse(0L, [&]
        {
            const std::string device = name8(deviceId);
            return m_core->isStageSequenceable(device.c_str()) ? m_core->getStageSequenceMaxLength(device.c_str()) : 0;
        });
    }

    bool MicroManagerProvider::startStageSequence(const QString& deviceId,
                                                  const std::vector<double>& positions,
                                                  QString* errorMessage)
    {
        return succeeds(errorMessage, [&]
        {
            const std::string device = name8(deviceId);
            m_core->loadStageSequence(device.c_str(), positions);
            m_core->startStageSequence(device.c_str());
        });
    }

    void MicroManagerProvider::stopStageSequence(const QString& deviceId)
    {
        succeeds(nullptr, [&] { m_core->stopStageSequence(name8(deviceId).c_str()); });
    }

    long MicroManagerProvider::propertySequenceCapacity(const QString& deviceId, const QString& name)
    {
        if (const CameraRef provider = camera(deviceId))
        {
            return provider.owner->propertySequenceCapacity(deviceId.trimmed(), name);
        }
        return micromanager::propertySequenceCapacity(*m_core, deviceId, name);
    }

    bool MicroManagerProvider::startPropertySequence(const QString& deviceId,
                                                     const QString& name,
                                                     const QStringList& values,
                                                     QString* errorMessage)
    {
        if (const CameraRef provider = camera(deviceId))
        {
            return provider.owner->startPropertySequence(deviceId.trimmed(), name, values, errorMessage);
        }
        return micromanager::startPropertySequence(*m_core, deviceId, name, values, errorMessage);
    }

    void MicroManagerProvider::stopPropertySequence(const QString& deviceId, const QString& name)
    {
        if (const CameraRef provider = camera(deviceId))
        {
            provider.owner->stopPropertySequence(deviceId.trimmed(), name);
            return;
        }
        micromanager::stopPropertySequence(*m_core, deviceId, name);
    }

    // The pixel size configurations are on the core of ScopeOne, also for a camera in a DriverHost.
    // The core scales them by the binning of the camera it holds, as MMCore does, so a camera in
    // a DriverHost is scaled by its own binning here.
    double MicroManagerProvider::pixelSizeUm(const QString& cameraId) const
    {
        const CameraEntry entry = cameraEntry(cameraId);
        if (!entry.camera)
        {
            return 0.0;
        }
        const double pixelSizeUm = orElse(0.0, [&] { return m_core->getPixelSizeUm(); });
        if (entry.local)
        {
            return pixelSizeUm;
        }
        QReadLocker locker(&m_camerasLock);
        return pixelSizeUm * m_binnings.value(cameraId.trimmed(), 1);
    }

    void MicroManagerProvider::readBinning(const QString& cameraId)
    {
        const CameraRef provider = camera(cameraId);
        // Cameras name their binning "2" or "2x2"
        const QString value = provider.owner->getProperty(cameraId, QLatin1String(MM::g_Keyword_Binning), false);
        const int binning = (std::max)(1, value.section(QLatin1Char('x'), 0, 0).toInt());
        QWriteLocker locker(&m_camerasLock);
        m_binnings.insert(cameraId, binning);
    }
}

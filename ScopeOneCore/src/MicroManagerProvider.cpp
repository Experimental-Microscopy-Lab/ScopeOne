#include "internal/MicroManagerProvider.h"

#include "MMCore.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <utility>
#include <vector>

namespace scopeone::core::internal
{
    namespace
    {
        QStringList toQStringList(const std::vector<std::string>& values)
        {
            QStringList result;
            result.reserve(static_cast<qsizetype>(values.size()));
            for (const std::string& value : values)
            {
                result.append(QString::fromStdString(value));
            }
            return result;
        }

        // MMCore takes device and property names as trimmed narrow strings
        std::string name8(const QString& name)
        {
            return name.trimmed().toStdString();
        }

        // Runs an MMCore query and returns the fallback when the core reports an error
        template <class T, class Call>
        T orElse(T fallback, Call&& call)
        {
            try
            {
                return call();
            }
            catch (const CMMError&)
            {
                return fallback;
            }
        }

        // Runs an MMCore command and reports a core error through errorMessage
        template <class Call>
        bool succeeds(QString* errorMessage, Call&& call)
        {
            if (errorMessage)
            {
                errorMessage->clear();
            }
            try
            {
                call();
                return true;
            }
            catch (const CMMError& error)
            {
                if (errorMessage)
                {
                    *errorMessage = QString::fromStdString(error.getMsg());
                }
                return false;
            }
        }
    }

    MicroManagerProvider::MicroManagerProvider(std::shared_ptr<CMMCore> core,
                                               CameraProvider* cameraProvider,
                                               CameraRuntimeControl* cameraRuntimeControl)
        : m_core(std::move(core))
          , m_cameraProvider(cameraProvider)
          , m_cameraRuntimeControl(cameraRuntimeControl)
    {
    }

    HardwareProviderDescriptor MicroManagerProvider::descriptor() const
    {
        return {
            QStringLiteral("micro-manager"),
            QStringLiteral("Micro-Manager"),
            QStringLiteral("MMCore")
        };
    }

    QList<HardwareDeviceDescriptor> MicroManagerProvider::devices() const
    {
        return m_devices;
    }

    void MicroManagerProvider::setDevices(const QList<HardwareDeviceDescriptor>& devices)
    {
        m_devices = devices;
    }

    bool MicroManagerProvider::isCamera(const QString& deviceId) const
    {
        const QString normalizedId = deviceId.trimmed();
        return std::any_of(m_devices.cbegin(), m_devices.cend(),
                           [&normalizedId](const HardwareDeviceDescriptor& device)
                           {
                               return device.logicalId == normalizedId
                                   && device.kind == HardwareDeviceKind::Camera;
                           });
    }

    void MicroManagerProvider::setFrameSink(FrameSink sink)
    {
        m_cameraProvider->setFrameSink(std::move(sink));
    }

    void MicroManagerProvider::setPreviewStateSink(PreviewStateSink sink)
    {
        m_cameraProvider->setPreviewStateSink(std::move(sink));
    }

    bool MicroManagerProvider::startPreview()
    {
        return m_cameraProvider->startPreview();
    }

    bool MicroManagerProvider::stopPreview()
    {
        return m_cameraProvider->stopPreview();
    }

    bool MicroManagerProvider::startPreviewFor(const QString& cameraId)
    {
        return m_cameraProvider->startPreviewFor(cameraId);
    }

    bool MicroManagerProvider::stopPreviewFor(const QString& cameraId)
    {
        return m_cameraProvider->stopPreviewFor(cameraId);
    }

    bool MicroManagerProvider::isPreviewRunning(const QString& cameraId) const
    {
        return m_cameraProvider->isPreviewRunning(cameraId);
    }

    bool MicroManagerProvider::getExposure(const QString& cameraIdOrAll, double& exposureMs) const
    {
        return m_cameraProvider->getExposure(cameraIdOrAll, exposureMs);
    }

    bool MicroManagerProvider::setExposure(const QString& cameraIdOrAll, double exposureMs)
    {
        return m_cameraProvider->setExposure(cameraIdOrAll, exposureMs);
    }

    QStringList MicroManagerProvider::listProperties(const QString& cameraId)
    {
        if (isCamera(cameraId))
        {
            return m_cameraProvider->listProperties(cameraId);
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
        if (isCamera(cameraId))
        {
            return m_cameraProvider->getProperty(cameraId, name, fromCache);
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
        if (isCamera(cameraId))
        {
            return m_cameraProvider->setProperty(cameraId, name, value, errorMessage);
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
        if (isCamera(cameraId))
        {
            return m_cameraProvider->getPropertyType(cameraId, name);
        }
        return orElse(QStringLiteral("Unknown"), [&]
        {
            switch (m_core->getPropertyType(name8(cameraId).c_str(), name8(name).c_str()))
            {
            case MM::String: return QStringLiteral("String");
            case MM::Float: return QStringLiteral("Float");
            case MM::Integer: return QStringLiteral("Integer");
            default: return QStringLiteral("Unknown");
            }
        });
    }

    bool MicroManagerProvider::isPropertyReadOnly(const QString& cameraId, const QString& name)
    {
        if (isCamera(cameraId))
        {
            return m_cameraProvider->isPropertyReadOnly(cameraId, name);
        }
        return orElse(true, [&]
        {
            return m_core->isPropertyReadOnly(name8(cameraId).c_str(), name8(name).c_str());
        });
    }

    bool MicroManagerProvider::isPropertyPreInit(const QString& cameraId, const QString& name)
    {
        if (isCamera(cameraId))
        {
            return m_cameraProvider->isPropertyPreInit(cameraId, name);
        }
        return orElse(false, [&]
        {
            return m_core->isPropertyPreInit(name8(cameraId).c_str(), name8(name).c_str());
        });
    }

    QStringList MicroManagerProvider::getAllowedPropertyValues(const QString& cameraId,
                                                               const QString& name)
    {
        if (isCamera(cameraId))
        {
            return m_cameraProvider->getAllowedPropertyValues(cameraId, name);
        }
        return orElse(QStringList{}, [&]
        {
            return toQStringList(
                m_core->getAllowedPropertyValues(name8(cameraId).c_str(), name8(name).c_str()));
        });
    }

    bool MicroManagerProvider::hasPropertyLimits(const QString& cameraId, const QString& name)
    {
        if (isCamera(cameraId))
        {
            return m_cameraProvider->hasPropertyLimits(cameraId, name);
        }
        return orElse(false, [&]
        {
            return m_core->hasPropertyLimits(name8(cameraId).c_str(), name8(name).c_str());
        });
    }

    double MicroManagerProvider::getPropertyLowerLimit(const QString& cameraId, const QString& name)
    {
        if (isCamera(cameraId))
        {
            return m_cameraProvider->getPropertyLowerLimit(cameraId, name);
        }
        return orElse(0.0, [&]
        {
            return m_core->getPropertyLowerLimit(name8(cameraId).c_str(), name8(name).c_str());
        });
    }

    double MicroManagerProvider::getPropertyUpperLimit(const QString& cameraId, const QString& name)
    {
        if (isCamera(cameraId))
        {
            return m_cameraProvider->getPropertyUpperLimit(cameraId, name);
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
        return m_cameraProvider->setROI(cameraId, x, y, width, height);
    }

    bool MicroManagerProvider::clearROI(const QString& cameraId)
    {
        return m_cameraProvider->clearROI(cameraId);
    }

    bool MicroManagerProvider::getROI(const QString& cameraId,
                                      int& x,
                                      int& y,
                                      int& width,
                                      int& height)
    {
        return m_cameraProvider->getROI(cameraId, x, y, width, height);
    }

    bool MicroManagerProvider::captureEventFrame(const QString& cameraId,
                                                 scopeone::core::ImageFrame& frame,
                                                 int timeoutMs)
    {
        return m_cameraProvider->captureEventFrame(cameraId, frame, timeoutMs);
    }

    bool MicroManagerProvider::supportsEventSequence(const QString& cameraId) const
    {
        return m_cameraProvider->supportsEventSequence(cameraId);
    }

    bool MicroManagerProvider::captureEventSequence(const QString& cameraId,
                                                    int count,
                                                    std::vector<scopeone::core::ImageFrame>& frames,
                                                    int timeoutMs)
    {
        return m_cameraProvider->captureEventSequence(cameraId, count, frames, timeoutMs);
    }

    void MicroManagerProvider::setFrameDeliveryPaused(const QStringList& cameraIds, bool paused)
    {
        m_cameraRuntimeControl->setFrameDeliveryPaused(cameraIds, paused);
    }

    bool MicroManagerProvider::setRecordingFrameDeliveryEnabled(const QStringList& cameraIds,
                                                                 bool enabled)
    {
        return m_cameraRuntimeControl->setRecordingFrameDeliveryEnabled(cameraIds, enabled);
    }

    bool MicroManagerProvider::setHighRateFrameDeliveryEnabled(const QStringList& cameraIds,
                                                                bool enabled)
    {
        return m_cameraRuntimeControl->setHighRateFrameDeliveryEnabled(cameraIds, enabled);
    }

    bool MicroManagerProvider::isProcessingFrameTokenCurrent(const QString& cameraId,
                                                              quint64 token)
    {
        return m_cameraRuntimeControl->isProcessingFrameTokenCurrent(cameraId, token);
    }

    void MicroManagerProvider::finishProcessingFrame(const QString& cameraId, quint64 token)
    {
        m_cameraRuntimeControl->finishProcessingFrame(cameraId, token);
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

    bool MicroManagerProvider::autoShutter(bool& enabled, QString* errorMessage) const
    {
        return succeeds(errorMessage, [&] { enabled = m_core->getAutoShutter(); });
    }

    bool MicroManagerProvider::setAutoShutter(bool enabled, QString* errorMessage)
    {
        return succeeds(errorMessage, [&] { m_core->setAutoShutter(enabled); });
    }

    bool MicroManagerProvider::setCoreShutterOpen(bool open, QString* errorMessage)
    {
        return succeeds(errorMessage, [&] { m_core->setShutterOpen(open); });
    }

    bool MicroManagerProvider::hasAutofocus() const
    {
        return orElse(false, [&] { return !m_core->getAutoFocusDevice().empty(); });
    }

    long MicroManagerProvider::propertySequenceCapacity(const QString& device, const QString& property) const
    {
        if (device.isEmpty() || property.isEmpty())
        {
            return 0;
        }
        return orElse(0L, [&]
        {
            const std::string label = device.toStdString();
            const std::string name = property.toStdString();
            return m_core->isPropertySequenceable(label.c_str(), name.c_str())
                ? m_core->getPropertySequenceMaxLength(label.c_str(), name.c_str())
                : 0;
        });
    }

    bool MicroManagerProvider::startPropertySequences(const std::vector<PropertySequence>& sequences,
                                                      QString* errorMessage)
    {
        std::size_t started = 0;
        const bool ok = succeeds(errorMessage, [&]
        {
            for (const PropertySequence& sequence : sequences)
            {
                const std::string label = sequence.device.toStdString();
                const std::string name = sequence.property.toStdString();
                std::vector<std::string> values;
                values.reserve(sequence.values.size());
                for (const QString& value : sequence.values)
                {
                    values.push_back(value.toStdString());
                }
                m_core->loadPropertySequence(label.c_str(), name.c_str(), values);
                m_core->startPropertySequence(label.c_str(), name.c_str());
                ++started;
            }
        });
        if (!ok)
        {
            // Do not leave the properties that were armed running
            stopPropertySequences({sequences.begin(), sequences.begin() + started});
        }
        return ok;
    }

    void MicroManagerProvider::stopPropertySequences(const std::vector<PropertySequence>& sequences)
    {
        for (const PropertySequence& sequence : sequences)
        {
            succeeds(nullptr, [&]
            {
                m_core->stopPropertySequence(sequence.device.toStdString().c_str(),
                                             sequence.property.toStdString().c_str());
            });
        }
    }

    long MicroManagerProvider::focusSequenceCapacity(const QString& focusDevice) const
    {
        if (focusDevice.isEmpty())
        {
            return 0;
        }
        return orElse(0L, [&]
        {
            const std::string label = focusDevice.toStdString();
            return m_core->isStageSequenceable(label.c_str())
                ? m_core->getStageSequenceMaxLength(label.c_str())
                : 0;
        });
    }

    bool MicroManagerProvider::startFocusSequence(const QString& focusDevice,
                                                  const std::vector<double>& positions,
                                                  QString* errorMessage)
    {
        return succeeds(errorMessage, [&]
        {
            const std::string label = focusDevice.toStdString();
            m_core->loadStageSequence(label.c_str(), positions);
            m_core->startStageSequence(label.c_str());
        });
    }

    void MicroManagerProvider::stopFocusSequence(const QString& focusDevice)
    {
        if (!focusDevice.isEmpty())
        {
            succeeds(nullptr, [&] { m_core->stopStageSequence(focusDevice.toStdString().c_str()); });
        }
    }

    bool MicroManagerProvider::runAutofocus(QString* errorMessage)
    {
        return succeeds(errorMessage, [&] { m_core->fullFocus(); });
    }
}

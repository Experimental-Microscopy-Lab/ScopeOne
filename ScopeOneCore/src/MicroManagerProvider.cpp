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

        void setError(QString* errorMessage, const CMMError& error)
        {
            if (errorMessage)
            {
                *errorMessage = QString::fromStdString(error.getMsg());
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
        if (m_cameraProvider)
        {
            m_cameraProvider->setFrameSink(std::move(sink));
        }
    }

    void MicroManagerProvider::setPreviewStateSink(PreviewStateSink sink)
    {
        if (m_cameraProvider)
        {
            m_cameraProvider->setPreviewStateSink(std::move(sink));
        }
    }

    bool MicroManagerProvider::startPreview()
    {
        return m_cameraProvider && m_cameraProvider->startPreview();
    }

    bool MicroManagerProvider::stopPreview()
    {
        return m_cameraProvider && m_cameraProvider->stopPreview();
    }

    bool MicroManagerProvider::startPreviewFor(const QString& cameraId)
    {
        return m_cameraProvider && m_cameraProvider->startPreviewFor(cameraId);
    }

    bool MicroManagerProvider::stopPreviewFor(const QString& cameraId)
    {
        return m_cameraProvider && m_cameraProvider->stopPreviewFor(cameraId);
    }

    bool MicroManagerProvider::isPreviewRunning(const QString& cameraId) const
    {
        return m_cameraProvider && m_cameraProvider->isPreviewRunning(cameraId);
    }

    bool MicroManagerProvider::getExposure(const QString& cameraIdOrAll, double& exposureMs) const
    {
        return m_cameraProvider && m_cameraProvider->getExposure(cameraIdOrAll, exposureMs);
    }

    bool MicroManagerProvider::setExposure(const QString& cameraIdOrAll, double exposureMs)
    {
        return m_cameraProvider && m_cameraProvider->setExposure(cameraIdOrAll, exposureMs);
    }

    QStringList MicroManagerProvider::listProperties(const QString& cameraId)
    {
        if (isCamera(cameraId))
        {
            return m_cameraProvider ? m_cameraProvider->listProperties(cameraId) : QStringList{};
        }
        if (!m_core)
        {
            return {};
        }
        try
        {
            return toQStringList(
                m_core->getDevicePropertyNames(cameraId.trimmed().toStdString().c_str()));
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    QString MicroManagerProvider::getProperty(const QString& cameraId,
                                              const QString& name,
                                              bool fromCache)
    {
        if (isCamera(cameraId))
        {
            return m_cameraProvider
                       ? m_cameraProvider->getProperty(cameraId, name, fromCache)
                       : QString{};
        }
        if (!m_core)
        {
            return {};
        }
        try
        {
            const std::string device = cameraId.trimmed().toStdString();
            const std::string property = name.trimmed().toStdString();
            return QString::fromStdString(
                fromCache
                    ? m_core->getPropertyFromCache(device.c_str(), property.c_str())
                    : m_core->getProperty(device.c_str(), property.c_str()));
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    bool MicroManagerProvider::setProperty(const QString& cameraId,
                                           const QString& name,
                                           const QString& value,
                                           QString* errorMessage)
    {
        if (isCamera(cameraId))
        {
            return m_cameraProvider
                && m_cameraProvider->setProperty(cameraId, name, value, errorMessage);
        }
        if (!m_core)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("MMCore not available");
            }
            return false;
        }
        try
        {
            const std::string device = cameraId.trimmed().toStdString();
            m_core->setProperty(device.c_str(),
                                name.trimmed().toStdString().c_str(),
                                value.toStdString().c_str());
            m_core->waitForDevice(device.c_str());
            m_core->updateSystemStateCache();
            return true;
        }
        catch (const CMMError& error)
        {
            setError(errorMessage, error);
            return false;
        }
    }

    QString MicroManagerProvider::getPropertyType(const QString& cameraId, const QString& name)
    {
        if (isCamera(cameraId))
        {
            return m_cameraProvider
                       ? m_cameraProvider->getPropertyType(cameraId, name)
                       : QStringLiteral("Unknown");
        }
        if (!m_core)
        {
            return QStringLiteral("Unknown");
        }
        try
        {
            const MM::PropertyType type = m_core->getPropertyType(
                cameraId.trimmed().toStdString().c_str(),
                name.trimmed().toStdString().c_str());
            switch (type)
            {
            case MM::String: return QStringLiteral("String");
            case MM::Float: return QStringLiteral("Float");
            case MM::Integer: return QStringLiteral("Integer");
            default: return QStringLiteral("Unknown");
            }
        }
        catch (const CMMError&)
        {
            return QStringLiteral("Unknown");
        }
    }

    bool MicroManagerProvider::isPropertyReadOnly(const QString& cameraId, const QString& name)
    {
        if (isCamera(cameraId))
        {
            return !m_cameraProvider || m_cameraProvider->isPropertyReadOnly(cameraId, name);
        }
        if (!m_core)
        {
            return true;
        }
        try
        {
            return m_core->isPropertyReadOnly(cameraId.trimmed().toStdString().c_str(),
                                              name.trimmed().toStdString().c_str());
        }
        catch (const CMMError&)
        {
            return true;
        }
    }

    bool MicroManagerProvider::isPropertyPreInit(const QString& cameraId, const QString& name)
    {
        if (isCamera(cameraId))
        {
            return m_cameraProvider && m_cameraProvider->isPropertyPreInit(cameraId, name);
        }
        if (!m_core)
        {
            return false;
        }
        try
        {
            return m_core->isPropertyPreInit(cameraId.trimmed().toStdString().c_str(),
                                             name.trimmed().toStdString().c_str());
        }
        catch (const CMMError&)
        {
            return false;
        }
    }

    QStringList MicroManagerProvider::getAllowedPropertyValues(const QString& cameraId,
                                                               const QString& name)
    {
        if (isCamera(cameraId))
        {
            return m_cameraProvider
                       ? m_cameraProvider->getAllowedPropertyValues(cameraId, name)
                       : QStringList{};
        }
        if (!m_core)
        {
            return {};
        }
        try
        {
            return toQStringList(m_core->getAllowedPropertyValues(
                cameraId.trimmed().toStdString().c_str(),
                name.trimmed().toStdString().c_str()));
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    bool MicroManagerProvider::hasPropertyLimits(const QString& cameraId, const QString& name)
    {
        if (isCamera(cameraId))
        {
            return m_cameraProvider && m_cameraProvider->hasPropertyLimits(cameraId, name);
        }
        if (!m_core)
        {
            return false;
        }
        try
        {
            return m_core->hasPropertyLimits(cameraId.trimmed().toStdString().c_str(),
                                             name.trimmed().toStdString().c_str());
        }
        catch (const CMMError&)
        {
            return false;
        }
    }

    double MicroManagerProvider::getPropertyLowerLimit(const QString& cameraId, const QString& name)
    {
        if (isCamera(cameraId))
        {
            return m_cameraProvider ? m_cameraProvider->getPropertyLowerLimit(cameraId, name) : 0.0;
        }
        if (!m_core)
        {
            return 0.0;
        }
        try
        {
            return m_core->getPropertyLowerLimit(cameraId.trimmed().toStdString().c_str(),
                                                 name.trimmed().toStdString().c_str());
        }
        catch (const CMMError&)
        {
            return 0.0;
        }
    }

    double MicroManagerProvider::getPropertyUpperLimit(const QString& cameraId, const QString& name)
    {
        if (isCamera(cameraId))
        {
            return m_cameraProvider ? m_cameraProvider->getPropertyUpperLimit(cameraId, name) : 0.0;
        }
        if (!m_core)
        {
            return 0.0;
        }
        try
        {
            return m_core->getPropertyUpperLimit(cameraId.trimmed().toStdString().c_str(),
                                                 name.trimmed().toStdString().c_str());
        }
        catch (const CMMError&)
        {
            return 0.0;
        }
    }

    bool MicroManagerProvider::setROI(const QString& cameraId,
                                      int x,
                                      int y,
                                      int width,
                                      int height)
    {
        return m_cameraProvider && m_cameraProvider->setROI(cameraId, x, y, width, height);
    }

    bool MicroManagerProvider::clearROI(const QString& cameraId)
    {
        return m_cameraProvider && m_cameraProvider->clearROI(cameraId);
    }

    bool MicroManagerProvider::getROI(const QString& cameraId,
                                      int& x,
                                      int& y,
                                      int& width,
                                      int& height)
    {
        return m_cameraProvider && m_cameraProvider->getROI(cameraId, x, y, width, height);
    }

    bool MicroManagerProvider::captureEventFrame(const QString& cameraId,
                                                 scopeone::core::ImageFrame& frame,
                                                 int timeoutMs)
    {
        return m_cameraProvider && m_cameraProvider->captureEventFrame(cameraId, frame, timeoutMs);
    }

    void MicroManagerProvider::setFrameDeliveryPaused(const QStringList& cameraIds, bool paused)
    {
        if (m_cameraRuntimeControl)
        {
            m_cameraRuntimeControl->setFrameDeliveryPaused(cameraIds, paused);
        }
    }

    bool MicroManagerProvider::setRecordingFrameDeliveryEnabled(const QStringList& cameraIds,
                                                                 bool enabled)
    {
        return !m_cameraRuntimeControl
            || m_cameraRuntimeControl->setRecordingFrameDeliveryEnabled(cameraIds, enabled);
    }

    bool MicroManagerProvider::setHighRateFrameDeliveryEnabled(const QStringList& cameraIds,
                                                                bool enabled)
    {
        return !m_cameraRuntimeControl
            || m_cameraRuntimeControl->setHighRateFrameDeliveryEnabled(cameraIds, enabled);
    }

    bool MicroManagerProvider::isProcessingFrameTokenCurrent(const QString& cameraId,
                                                              quint64 token)
    {
        return m_cameraRuntimeControl
            && m_cameraRuntimeControl->isProcessingFrameTokenCurrent(cameraId, token);
    }

    void MicroManagerProvider::finishProcessingFrame(const QString& cameraId, quint64 token)
    {
        if (m_cameraRuntimeControl)
        {
            m_cameraRuntimeControl->finishProcessingFrame(cameraId, token);
        }
    }

    QString MicroManagerProvider::defaultXYStage() const
    {
        if (!m_core)
        {
            return {};
        }
        try
        {
            return QString::fromStdString(m_core->getXYStageDevice());
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    QString MicroManagerProvider::defaultZStage() const
    {
        if (!m_core)
        {
            return {};
        }
        try
        {
            return QString::fromStdString(m_core->getFocusDevice());
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    bool MicroManagerProvider::getXYPosition(const QString& deviceId,
                                             double& x,
                                             double& y,
                                             QString* errorMessage) const
    {
        x = 0.0;
        y = 0.0;
        if (!m_core)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("MMCore not available");
            }
            return false;
        }
        try
        {
            m_core->getXYPosition(deviceId.trimmed().toStdString().c_str(), x, y);
            return true;
        }
        catch (const CMMError& error)
        {
            setError(errorMessage, error);
            return false;
        }
    }

    bool MicroManagerProvider::getZPosition(const QString& deviceId,
                                            double& z,
                                            QString* errorMessage) const
    {
        z = 0.0;
        if (!m_core)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("MMCore not available");
            }
            return false;
        }
        try
        {
            z = m_core->getPosition(deviceId.trimmed().toStdString().c_str());
            return true;
        }
        catch (const CMMError& error)
        {
            setError(errorMessage, error);
            return false;
        }
    }

    bool MicroManagerProvider::setRelativeXYPosition(const QString& deviceId,
                                                     double dx,
                                                     double dy,
                                                     QString* errorMessage)
    {
        if (!m_core)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("MMCore not available");
            }
            return false;
        }
        try
        {
            const std::string device = deviceId.trimmed().toStdString();
            m_core->setRelativeXYPosition(device.c_str(), dx, dy);
            m_core->waitForDevice(device.c_str());
            return true;
        }
        catch (const CMMError& error)
        {
            setError(errorMessage, error);
            return false;
        }
    }

    bool MicroManagerProvider::setRelativeZPosition(const QString& deviceId,
                                                    double dz,
                                                    QString* errorMessage)
    {
        if (!m_core)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("MMCore not available");
            }
            return false;
        }
        try
        {
            const std::string device = deviceId.trimmed().toStdString();
            m_core->setRelativePosition(device.c_str(), dz);
            m_core->waitForDevice(device.c_str());
            return true;
        }
        catch (const CMMError& error)
        {
            setError(errorMessage, error);
            return false;
        }
    }

    bool MicroManagerProvider::setXYPosition(const QString& deviceId,
                                             double x,
                                             double y,
                                             QString* errorMessage)
    {
        if (!m_core)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("MMCore not available");
            }
            return false;
        }
        try
        {
            const std::string device = deviceId.trimmed().toStdString();
            m_core->setXYPosition(device.c_str(), x, y);
            m_core->waitForDevice(device.c_str());
            return true;
        }
        catch (const CMMError& error)
        {
            setError(errorMessage, error);
            return false;
        }
    }

    bool MicroManagerProvider::setZPosition(const QString& deviceId,
                                            double z,
                                            QString* errorMessage)
    {
        if (!m_core)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("MMCore not available");
            }
            return false;
        }
        try
        {
            const std::string device = deviceId.trimmed().toStdString();
            m_core->setPosition(device.c_str(), z);
            m_core->waitForDevice(device.c_str());
            return true;
        }
        catch (const CMMError& error)
        {
            setError(errorMessage, error);
            return false;
        }
    }

    bool MicroManagerProvider::isShutterOpen(const QString& deviceId,
                                             bool& open,
                                             QString* errorMessage) const
    {
        open = false;
        if (!m_core)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("MMCore not available");
            }
            return false;
        }
        try
        {
            open = m_core->getShutterOpen(deviceId.trimmed().toStdString().c_str());
            return true;
        }
        catch (const CMMError& error)
        {
            setError(errorMessage, error);
            return false;
        }
    }

    bool MicroManagerProvider::setShutterOpen(const QString& deviceId,
                                              bool open,
                                              QString* errorMessage)
    {
        if (!m_core)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("MMCore not available");
            }
            return false;
        }
        try
        {
            const std::string device = deviceId.trimmed().toStdString();
            m_core->setShutterOpen(device.c_str(), open);
            m_core->waitForDevice(device.c_str());
            m_core->updateSystemStateCache();
            return true;
        }
        catch (const CMMError& error)
        {
            setError(errorMessage, error);
            return false;
        }
    }

    bool MicroManagerProvider::getState(const QString& deviceId,
                                        long& state,
                                        QString* errorMessage) const
    {
        state = 0;
        if (!m_core)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("MMCore not available");
            }
            return false;
        }
        try
        {
            state = m_core->getState(deviceId.trimmed().toStdString().c_str());
            return true;
        }
        catch (const CMMError& error)
        {
            setError(errorMessage, error);
            return false;
        }
    }

    bool MicroManagerProvider::setState(const QString& deviceId,
                                        long state,
                                        QString* errorMessage)
    {
        if (!m_core)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("MMCore not available");
            }
            return false;
        }
        try
        {
            const std::string device = deviceId.trimmed().toStdString();
            m_core->setState(device.c_str(), state);
            m_core->waitForDevice(device.c_str());
            m_core->updateSystemStateCache();
            return true;
        }
        catch (const CMMError& error)
        {
            setError(errorMessage, error);
            return false;
        }
    }

    QString MicroManagerProvider::stateLabel(const QString& deviceId, long state) const
    {
        if (!m_core || state < 0)
        {
            return {};
        }
        try
        {
            const std::vector<std::string> labels =
                m_core->getStateLabels(deviceId.trimmed().toStdString().c_str());
            const size_t index = static_cast<size_t>(state);
            return index < labels.size() ? QString::fromStdString(labels[index]) : QString{};
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    QStringList MicroManagerProvider::availableConfigGroups() const
    {
        if (!m_core)
        {
            return {};
        }
        try
        {
            return toQStringList(m_core->getAvailableConfigGroups());
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    QStringList MicroManagerProvider::availableConfigs(const QString& groupName) const
    {
        if (!m_core)
        {
            return {};
        }
        try
        {
            return toQStringList(
                m_core->getAvailableConfigs(groupName.trimmed().toStdString().c_str()));
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    QString MicroManagerProvider::currentConfig(const QString& groupName) const
    {
        if (!m_core)
        {
            return {};
        }
        try
        {
            const std::string group = groupName.trimmed().toStdString();
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
        }
        catch (const CMMError&)
        {
        }
        return {};
    }

    bool MicroManagerProvider::setConfig(const QString& groupName,
                                         const QString& configName,
                                         QString* errorMessage)
    {
        if (errorMessage)
        {
            errorMessage->clear();
        }
        if (!m_core)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("MMCore not available");
            }
            return false;
        }

        try
        {
            const std::string group = groupName.trimmed().toStdString();
            const std::string config = configName.trimmed().toStdString();
            const Configuration preset = m_core->getConfigData(group.c_str(), config.c_str());
            std::vector<PropertySetting> pending;
            pending.reserve(preset.size());
            for (size_t index = 0; index < preset.size(); ++index)
            {
                pending.push_back(preset.getSetting(index));
            }

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
                if (failed.empty())
                {
                    break;
                }
                if (failed.size() == pending.size())
                {
                    if (errorMessage)
                    {
                        *errorMessage = QStringLiteral("Failed to apply config preset %1 = %2 at %3")
                                            .arg(groupName, configName, failureDescription);
                    }
                    return false;
                }
                pending = std::move(failed);
            }

            m_core->waitForSystem();
            m_core->updateSystemStateCache();
            return true;
        }
        catch (const CMMError& error)
        {
            setError(errorMessage, error);
            return false;
        }
    }
}

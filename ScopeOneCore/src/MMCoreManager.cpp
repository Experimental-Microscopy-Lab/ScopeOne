#include "internal/MMCoreManager.h"
#include "internal/CameraManager.h"
#include <QFile>
#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QTextStream>
#include <algorithm>
#include <vector>

namespace scopeone::core::internal
{
    namespace
    {
        struct DevicePropertyState
        {
            QStringList preInitProperties;
            QStringList properties;
        };

        struct ConfigProperty
        {
            QString name;
            QString value;
        };

        // Sets adapter search paths relative to the application directory
        void configureAdapterSearchPaths(CMMCore& core, const QStringList& additionalPaths)
        {
            const QString appDir = QCoreApplication::applicationDirPath();
            QStringList paths{appDir};
            for (const QString& path : additionalPaths)
            {
                const QString normalizedPath = QDir::cleanPath(path.trimmed());
                if (!normalizedPath.isEmpty() && !paths.contains(normalizedPath, Qt::CaseInsensitive))
                {
                    paths.append(normalizedPath);
                }
            }

            std::vector<std::string> searchPaths;
            searchPaths.reserve(static_cast<size_t>(paths.size()));
            for (const QString& path : paths)
            {
                searchPaths.push_back(path.toStdString());
            }
            core.setDeviceAdapterSearchPaths(searchPaths);
        }

        // Encodes one property replay entry as compact JSON
        QString encodePropertyPayload(const QString& name, const QString& value)
        {
            QJsonObject property;
            property.insert(QStringLiteral("name"), name);
            property.insert(QStringLiteral("value"), value);
            return QString::fromUtf8(QJsonDocument(property).toJson(QJsonDocument::Compact));
        }

        // Splits one configuration line into command fields
        QStringList splitConfigLine(const QString& line)
        {
            QStringList parts;
            int start = 0;
            for (int i = 0; i < 3; ++i)
            {
                const int comma = line.indexOf(QLatin1Char(','), start);
                if (comma < 0)
                {
                    return {};
                }
                parts.append(line.mid(start, comma - start).trimmed());
                start = comma + 1;
            }
            parts.append(line.mid(start).trimmed());
            return parts;
        }

        // Reads explicit property entries from a configuration file
        QHash<QString, QList<ConfigProperty>> explicitConfigProperties(const QString& configPath)
        {
            QHash<QString, QList<ConfigProperty>> properties;
            QFile file(configPath);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            {
                return properties;
            }

            QTextStream stream(&file);
            while (!stream.atEnd())
            {
                const QString line = stream.readLine().trimmed();
                if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                {
                    continue;
                }

                const QStringList parts = splitConfigLine(line);
                if (parts.size() != 4 || parts[0] != QStringLiteral("Property"))
                {
                    continue;
                }

                const QString deviceLabel = parts[1];
                const QString propertyName = parts[2];
                const QString propertyValue = parts[3];
                if (!deviceLabel.isEmpty() && !propertyName.isEmpty() && !propertyValue.isEmpty())
                {
                    properties[deviceLabel].append(ConfigProperty{propertyName, propertyValue});
                }
            }
            return properties;
        }

        // Separates replayable config properties by initialization phase
        DevicePropertyState prepareConfigPropertyReplay(CMMCore& core,
                                                        const QString& deviceLabel,
                                                        const QList<ConfigProperty>& properties)
        {
            DevicePropertyState state;
            const QString trimmedLabel = deviceLabel.trimmed();
            if (trimmedLabel.isEmpty() || properties.isEmpty())
            {
                return state;
            }

            const std::string label = trimmedLabel.toStdString();
            try
            {
                for (const ConfigProperty& configProperty : properties)
                {
                    const QString propertyName = configProperty.name.trimmed();
                    const QString propertyValue = configProperty.value.trimmed();

                    if (propertyName.isEmpty() || propertyValue.isEmpty())
                    {
                        continue;
                    }

                    const std::string property = propertyName.toStdString();
                    bool preInit = false;
                    try
                    {
                        preInit = core.isPropertyPreInit(label.c_str(), property.c_str());
                    }
                    catch (const CMMError&)
                    {
                    }

                    bool readOnly = false;
                    try
                    {
                        readOnly = core.isPropertyReadOnly(label.c_str(), property.c_str());
                    }
                    catch (const CMMError&)
                    {
                    }

                    if (!preInit && readOnly)
                    {
                        continue;
                    }

                    const QString encodedProperty = encodePropertyPayload(propertyName, propertyValue);
                    if (preInit)
                    {
                        state.preInitProperties.append(encodedProperty);
                    }
                    else
                    {
                        state.properties.append(encodedProperty);
                    }
                }
            }
            catch (const CMMError& error)
            {
                qWarning().noquote()
                    << QString("Failed to prepare cfg property replay for '%1': %2")
                    .arg(trimmedLabel, QString::fromStdString(error.getMsg()));
            }

            return state;
        }
    } // namespace

    // Loads one config file into MMCore
    bool loadConfigurationFile(CMMCore& core,
                               const QString& configPath,
                               const QStringList& additionalPaths,
                               QString* errorMessage)
    {
        if (configPath.trimmed().isEmpty())
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Configuration path is empty");
            }
            return false;
        }

        try
        {
            configureAdapterSearchPaths(core, additionalPaths);
            core.loadSystemConfiguration(configPath.toStdString().c_str());
            return true;
        }
        catch (const CMMError& e)
        {
            if (errorMessage)
            {
                *errorMessage = QString::fromStdString(e.getMsg());
            }
            return false;
        }
    }

    // Reads labels for devices currently loaded in MMCore
    QStringList loadedDeviceLabels(CMMCore& core, QString* errorMessage)
    {
        try
        {
            QStringList loadedDevices;
            const std::vector<std::string> loaded = core.getLoadedDevices();
            for (const auto& dev : loaded)
            {
                loadedDevices.append(QString::fromStdString(dev));
            }
            return loadedDevices;
        }
        catch (const CMMError& e)
        {
            if (errorMessage)
            {
                *errorMessage = QString::fromStdString(e.getMsg());
            }
            return {};
        }
    }

    // Reads camera metadata before backend startup
    QList<MMCoreManager::CameraLoadInfo> loadedCameraInfos(
        CMMCore& core,
        const QStringList& loadedDevices,
        const QHash<QString, QList<ConfigProperty>>& explicitProperties)
    {
        QList<MMCoreManager::CameraLoadInfo> cameras;
        cameras.reserve(loadedDevices.size());
        for (const QString& deviceName : loadedDevices)
        {
            try
            {
                const MM::DeviceType deviceType = core.getDeviceType(deviceName.toStdString().c_str());
                if (deviceType != MM::CameraDevice)
                {
                    continue;
                }

                MMCoreManager::CameraLoadInfo info;
                info.label = deviceName;
                try
                {
                    info.adapter = QString::fromStdString(core.getDeviceLibrary(deviceName.toStdString().c_str()));
                }
                catch (const CMMError&)
                {
                }
                try
                {
                    info.device = QString::fromStdString(core.getDeviceName(deviceName.toStdString().c_str()));
                }
                catch (const CMMError&)
                {
                }
                try
                {
                    core.setCameraDevice(deviceName.toStdString().c_str());
                    const double exposure = core.getExposure();
                    if (exposure > 0.0)
                    {
                        info.exposureMs = exposure;
                    }
                }
                catch (const CMMError&)
                {
                }
                const DevicePropertyState propertyState = prepareConfigPropertyReplay(
                    core,
                    deviceName,
                    explicitProperties.value(deviceName));
                info.preInitProperties = propertyState.preInitProperties;
                info.properties = propertyState.properties;

                cameras.append(std::move(info));
            }
            catch (const CMMError&)
            {
            }
        }
        return cameras;
    }

    // Creates the MMCore manager and backing core instance
    MMCoreManager::MMCoreManager(QObject* parent)
        : QObject(parent)
          , m_mmcore(std::make_shared<CMMCore>())
    {
    }

    // Loads and initializes devices without creating Qt camera backends
    bool MMCoreManager::loadConfigurationDevices(const QString& configPath,
                                                 LoadConfigResult& result,
                                                 QString& errorMessage)
    {
        result = LoadConfigResult{};
        if (!loadConfigurationFile(
                *m_mmcore, configPath, m_additionalDeviceAdapterSearchPaths, &errorMessage))
        {
            return false;
        }

        QString listError;
        const QStringList loadedDevices = loadedDeviceLabels(*m_mmcore, &listError);
        if (!listError.isEmpty())
        {
            qWarning().noquote() << QString("Failed to query loaded devices: %1").arg(listError);
        }
        const QHash<QString, QList<ConfigProperty>> explicitProperties = explicitConfigProperties(configPath);
        const QList<CameraLoadInfo> cameraInfos =
            loadedCameraInfos(*m_mmcore, loadedDevices, explicitProperties);
        const bool useSingleCamera = cameraInfos.size() == 1;

        int successCount = 0;
        int failCount = 0;
        int skippedCameraCount = 0;
        for (const QString& deviceName : loadedDevices)
        {
            try
            {
                bool isCamera = false;
                try
                {
                    const MM::DeviceType deviceType = m_mmcore->getDeviceType(deviceName.toStdString().c_str());
                    isCamera = (deviceType == MM::CameraDevice);
                }
                catch (const CMMError&)
                {
                    isCamera = false;
                }

                if (isCamera && !useSingleCamera)
                {
                    skippedCameraCount++;
                    continue;
                }

                const DeviceInitializationState state = m_mmcore->getDeviceInitializationState(
                    deviceName.toStdString().c_str());
                if (state != InitializedSuccessfully)
                {
                    try
                    {
                        m_mmcore->initializeDevice(deviceName.toStdString().c_str());
                        successCount++;
                    }
                    catch (const CMMError&)
                    {
                        failCount++;
                    }
                }
                else
                {
                    successCount++;
                }
            }
            catch (const CMMError&)
            {
                failCount++;
            }
        }

        if (!useSingleCamera)
        {
            for (const auto& ci : cameraInfos)
            {
                try
                {
                    const std::vector<std::string> current = m_mmcore->getLoadedDevices();
                    const bool stillLoaded =
                        std::find(current.begin(), current.end(), ci.label.toStdString()) != current.end();
                    if (stillLoaded)
                    {
                        m_mmcore->unloadDevice(ci.label.toStdString().c_str());
                    }
                }
                catch (const CMMError&)
                {
                }
            }
        }

        result.cameras = cameraInfos;
        result.successCount = successCount;
        result.failCount = failCount;
        result.skippedCameraCount = skippedCameraCount;
        result.foundCamera = !cameraInfos.isEmpty();
        result.useSingleCamera = useSingleCamera;
        return true;
    }

    // Creates camera backends on the owning Qt thread
    void MMCoreManager::startCameraBackends(CameraManager& cameraManager,
                                            LoadConfigResult& result)
    {
        result.cameraIds.clear();
        for (const CameraLoadInfo& camera : result.cameras)
        {
            const bool started = result.useSingleCamera
                ? cameraManager.configureNativeCamera(m_mmcore, camera.label, camera.exposureMs)
                : cameraManager.addAgentCamera(camera.label,
                                               camera.adapter,
                                               camera.device,
                                               camera.preInitProperties,
                                               camera.properties,
                                               camera.exposureMs);
            if (!started)
            {
                ++result.failCount;
                continue;
            }

            result.cameraIds.append(camera.label);
        }
    }

} // namespace scopeone::core::internal

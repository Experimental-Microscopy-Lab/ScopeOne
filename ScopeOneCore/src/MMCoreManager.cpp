#include "internal/MMCoreManager.h"
#include "internal/MultiProcessCameraManager.h"
#include <QDebug>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <vector>

namespace scopeone::core::internal
{
    namespace
    {
        struct CameraLoadInfo
        {
            QString label;
            QString adapter;
            QString device;
            QStringList preInitProperties;
            QStringList properties;
            double exposureMs{10.0};
        };

        struct DevicePropertyState
        {
            QStringList preInitProperties;
            QStringList properties;
        };

        void configureAdapterSearchPaths(CMMCore& core)
        {
            const QString appDir = QCoreApplication::applicationDirPath();
            std::vector<std::string> searchPaths;
            searchPaths.push_back(appDir.toStdString());
            core.setDeviceAdapterSearchPaths(searchPaths);
        }

        QString encodePropertyPayload(const QString& name, const QString& value)
        {
            QJsonObject property;
            property.insert(QStringLiteral("name"), name);
            property.insert(QStringLiteral("value"), value);
            return QString::fromUtf8(QJsonDocument(property).toJson(QJsonDocument::Compact));
        }

        DevicePropertyState captureDevicePropertyState(CMMCore& core, const QString& deviceLabel)
        {
            DevicePropertyState state;
            const QString trimmedLabel = deviceLabel.trimmed();
            if (trimmedLabel.isEmpty())
            {
                return state;
            }

            const std::string label = trimmedLabel.toStdString();
            try
            {
                const auto names = core.getDevicePropertyNames(label.c_str());
                for (const auto& rawName : names)
                {
                    const QString propertyName = QString::fromStdString(rawName).trimmed();
                    if (propertyName.isEmpty())
                    {
                        continue;
                    }

                    const std::string property = propertyName.toStdString();

                    QString value;
                    try
                    {
                        value = QString::fromStdString(core.getProperty(label.c_str(), property.c_str()));
                    }
                    catch (const CMMError&)
                    {
                        continue;
                    }

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

                    const QString encodedProperty = encodePropertyPayload(propertyName, value);
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
                    << QString("Failed to snapshot properties for '%1': %2")
                           .arg(trimmedLabel, QString::fromStdString(error.getMsg()));
            }

            return state;
        }
    } // namespace

    bool loadConfigurationFile(CMMCore& core, const QString& configPath, QString* errorMessage)
    {
        // Load one Micro-Manager config file into MMCore
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
            configureAdapterSearchPaths(core);
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

    std::vector<CameraLoadInfo> loadedCameraInfos(CMMCore& core,
                                                  const QStringList& loadedDevices)
    {
        // Read camera metadata before backend startup
        std::vector<CameraLoadInfo> cameras;
        cameras.reserve(static_cast<size_t>(loadedDevices.size()));
        for (const QString& deviceName : loadedDevices)
        {
            try
            {
                const MM::DeviceType deviceType = core.getDeviceType(deviceName.toStdString().c_str());
                if (deviceType != MM::CameraDevice)
                {
                    continue;
                }

                CameraLoadInfo info;
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
                const DevicePropertyState propertyState = captureDevicePropertyState(core, deviceName);
                info.preInitProperties = propertyState.preInitProperties;
                info.properties = propertyState.properties;

                cameras.push_back(std::move(info));
            }
            catch (const CMMError&)
            {
            }
        }
        return cameras;
    }

    MMCoreManager::MMCoreManager(QObject* parent)
        : QObject(parent)
          , m_mmcore(std::make_shared<CMMCore>())
    {
        qRegisterMetaType<DeviceType>("DeviceType");
    }

    QString MMCoreManager::getDeviceTypeString(DeviceType type) const
    {
        switch (type)
        {
        case DeviceType::CameraDevice: return "Camera";
        case DeviceType::ShutterDevice: return "Shutter";
        case DeviceType::StateDevice: return "State";
        case DeviceType::StageDevice: return "Stage";
        case DeviceType::XYStageDevice: return "XYStage";
        case DeviceType::SerialDevice: return "Serial";
        case DeviceType::GenericDevice: return "Generic";
        case DeviceType::AutoFocusDevice: return "AutoFocus";
        case DeviceType::CoreDevice: return "Core";
        case DeviceType::ImageProcessorDevice: return "ImageProcessor";
        case DeviceType::SignalIODevice: return "SignalIO";
        case DeviceType::MagnifierDevice: return "Magnifier";
        case DeviceType::SLMDevice: return "SLM";
        case DeviceType::GalvoDevice: return "Galvo";
        case DeviceType::HubDevice: return "Hub";
        case DeviceType::UnknownType: return "Unknown";
        case DeviceType::AnyType: return "Any";
        default: return "Unknown";
        }
    }

    bool MMCoreManager::loadConfigurationAndStartCameras(const QString& configPath,
                                                         MultiProcessCameraManager* mpcm,
                                                         LoadConfigResult* result,
                                                         QString* errorMessage)
    {
        // Initialize devices and choose native or agent camera startup
        if (result)
        {
            *result = LoadConfigResult{};
        }
        if (!m_mmcore)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("MMCore not available");
            }
            return false;
        }

        QString loadError;
        if (!loadConfigurationFile(*m_mmcore, configPath, &loadError))
        {
            if (errorMessage)
            {
                *errorMessage = loadError;
            }
            return false;
        }

        QString listError;
        const QStringList loadedDevices = loadedDeviceLabels(*m_mmcore, &listError);
        if (!listError.isEmpty())
        {
            qWarning().noquote() << QString("Failed to query loaded devices: %1").arg(listError);
        }
        const std::vector<CameraLoadInfo> cameraInfos =
            loadedCameraInfos(*m_mmcore, loadedDevices);
        const bool useSingleCamera = (cameraInfos.size() == 1);

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

        QStringList cameraIds;
        QStringList agentsStarted;
        const bool foundCamera = !cameraInfos.empty();

        if (useSingleCamera && mpcm)
        {
            mpcm->setNativeCore(m_mmcore);
        }

        for (const auto& ci : cameraInfos)
        {
            if (!mpcm)
            {
                cameraIds.append(ci.label);
                continue;
            }

            if (useSingleCamera)
            {
                if (mpcm->startSingleCamera(ci.label, ci.exposureMs))
                {
                    cameraIds.append(ci.label);
                }
                else
                {
                    failCount++;
                }
            }
            else
            {
                if (mpcm->startAgentFor(ci.label,
                                        ci.adapter,
                                        ci.device,
                                        ci.preInitProperties,
                                        ci.properties,
                                        ci.exposureMs))
                {
                    cameraIds.append(ci.label);
                    agentsStarted.append(ci.label);
                }
                else
                {
                    failCount++;
                }
            }
        }

        if (result)
        {
            result->cameraIds = cameraIds;
            result->agentsStarted = agentsStarted;
            result->successCount = successCount;
            result->failCount = failCount;
            result->skippedCameraCount = skippedCameraCount;
            result->foundCamera = foundCamera;
        }
        return true;
    } // namespace scopeone::core::internal
}

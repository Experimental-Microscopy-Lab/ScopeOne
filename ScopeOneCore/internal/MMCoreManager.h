#pragma once

#include <QObject>
#include <QStringList>
#include <memory>
#include "MMCore.h"

namespace scopeone::core::internal
{
    class MultiProcessCameraManager;

    enum class DeviceType
    {
        UnknownType = 0,
        AnyType,
        CameraDevice,
        ShutterDevice,
        StateDevice,
        StageDevice,
        XYStageDevice,
        SerialDevice,
        GenericDevice,
        AutoFocusDevice,
        CoreDevice,
        ImageProcessorDevice,
        SignalIODevice,
        MagnifierDevice,
        SLMDevice,
        GalvoDevice,
        HubDevice
    };

    class MMCoreManager : public QObject
    {
        Q_OBJECT

    public:
        struct LoadConfigResult
        {
            QStringList cameraIds;
            QStringList agentsStarted;
            int successCount{0};
            int failCount{0};
            int skippedCameraCount{0};
            bool foundCamera{false};
        };

        explicit MMCoreManager(QObject* parent = nullptr);
        ~MMCoreManager() override = default;

        std::shared_ptr<CMMCore> getCore() const { return m_mmcore; }

        QString getDeviceTypeString(DeviceType type) const;
        bool loadConfigurationAndStartCameras(const QString& configPath,
                                              MultiProcessCameraManager* mpcm,
                                              LoadConfigResult* result,
                                              QString* errorMessage);

    private:
        std::shared_ptr<CMMCore> m_mmcore;
    };
}

Q_DECLARE_METATYPE(scopeone::core::internal::DeviceType)

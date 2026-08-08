#pragma once

#include <QObject>
#include <QList>
#include <QStringList>
#include <memory>
#include "MMCore.h"

namespace scopeone::core::internal
{
    class CameraManager;

    class MMCoreManager : public QObject
    {
        Q_OBJECT

    public:
        struct CameraLoadInfo
        {
            QString label;
            QString adapter;
            QString device;
            QStringList preInitProperties;
            QStringList properties;
            double exposureMs{10.0};
        };

        struct LoadConfigResult
        {
            QStringList cameraIds;
            QList<CameraLoadInfo> cameras;
            QStringList failedDevices;
            int successCount{0};
            int failCount{0};
            int skippedCameraCount{0};
            bool foundCamera{false};
            bool useSingleCamera{false};
        };

        explicit MMCoreManager(QObject* parent = nullptr);
        ~MMCoreManager() override = default;

        std::shared_ptr<CMMCore> getCore() const { return m_mmcore; }
        const QStringList& additionalDeviceAdapterSearchPaths() const
        {
            return m_additionalDeviceAdapterSearchPaths;
        }
        void setAdditionalDeviceAdapterSearchPaths(const QStringList& paths)
        {
            m_additionalDeviceAdapterSearchPaths = paths;
        }

        bool loadConfigurationDevices(const QString& configPath,
                                      LoadConfigResult& result,
                                      QString& errorMessage);
        bool startCameraBackends(CameraManager& cameraManager,
                                 LoadConfigResult& result);
    private:
        std::shared_ptr<CMMCore> m_mmcore;
        QStringList m_additionalDeviceAdapterSearchPaths;
    };
}

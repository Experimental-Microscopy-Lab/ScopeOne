#pragma once

#include <QObject>
#include <QString>

#include "scopeone/CameraProvider.h"

namespace scopeone::core::internal
{
    class DeviceRegistry;

    class AcquisitionEngine : public QObject
    {
        Q_OBJECT

    public:
        AcquisitionEngine(DeviceRegistry* deviceRegistry, QObject* parent = nullptr);

        bool start(const QString& cameraIdOrAll);
        bool stop(const QString& cameraIdOrAll);

    private:
        DeviceRegistry* m_deviceRegistry{nullptr};
    };
}

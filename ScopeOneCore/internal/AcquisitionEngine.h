#pragma once

#include <QString>

#include "scopeone/CameraProvider.h"

namespace scopeone::core::internal
{
    class DeviceRegistry;

    class AcquisitionEngine
    {
    public:
        explicit AcquisitionEngine(DeviceRegistry& deviceRegistry);

        bool start(const QString& cameraIdOrAll);
        bool stop(const QString& cameraIdOrAll);

    private:
        DeviceRegistry& m_deviceRegistry;
    };
}

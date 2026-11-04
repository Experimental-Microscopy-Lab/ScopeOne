#pragma once

#include "internal/DaqDeviceManager.h"

namespace scopeone::core::internal
{
    class NIDaqmxDeviceFactory final : public DaqDeviceFactory
    {
    public:
        QList<scopeone::core::DaqDeviceDescriptor> devices() const override;
        scopeone::core::DaqController* createController(
            const QString& deviceId,
            QObject* parent = nullptr) override;
    };
}

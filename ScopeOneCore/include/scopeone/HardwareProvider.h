#pragma once

#include <QList>
#include <QString>

#include <memory>

#include "scopeone/HardwareTypes.h"

namespace scopeone::core
{
    class HardwareProvider
    {
    public:
        virtual ~HardwareProvider() = default;

        virtual HardwareProviderDescriptor descriptor() const = 0;
        virtual QList<HardwareDeviceDescriptor> devices() const = 0;
    };

    using HardwareProviderPtr = std::shared_ptr<HardwareProvider>;
}

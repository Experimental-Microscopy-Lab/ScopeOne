#pragma once

#include <QList>
#include <QString>

#include <memory>

#include "scopeone/HardwareTypes.h"

namespace scopeone::core
{
    class SCOPEONE_CORE_EXPORT HardwareProvider
    {
    public:
        virtual ~HardwareProvider();

        virtual HardwareProviderDescriptor descriptor() const = 0;
        virtual QList<HardwareDeviceDescriptor> devices() const = 0;
    };

    using HardwareProviderPtr = std::shared_ptr<HardwareProvider>;
}

#pragma once

#include <QJsonObject>
#include <QtPlugin>

#include "scopeone/HardwareProvider.h"

namespace scopeone::core
{
    // The entry point of a driver library. ScopeOne_DriverHost loads the library, gets this
    // object from Qt's plugin loader and asks it for the provider of the devices.
    class Driver
    {
    public:
        virtual ~Driver() = default;

        virtual QString providerId() const = 0;
        virtual HardwareProviderPtr createProvider(const QJsonObject& options,
                                                   QString* errorMessage = nullptr) = 0;
    };
}

#define ScopeOneDriver_iid "org.scopeone.Driver/1.0"
Q_DECLARE_INTERFACE(scopeone::core::Driver, ScopeOneDriver_iid)

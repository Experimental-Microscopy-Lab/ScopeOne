#pragma once

#include <QStringList>
#include <QVariantMap>
#include <QtPlugin>

#include "scopeone/Device.h"

namespace scopeone::core
{
    // The entry point of a driver library. ScopeOne_DriverHost loads the library, gets this
    // object from Qt's plugin loader and asks it to create one device by name. A library may
    // offer several devices, as an adapter library of Micro-Manager does.
    class Driver
    {
    public:
        virtual ~Driver() = default;

        virtual QString driverId() const = 0;
        virtual QStringList deviceNames() const = 0;
        // The pre-init properties are the values a device needs before it can start, such as a
        // port, and come from the configuration of the system
        virtual DevicePtr createDevice(const QString& name,
                                       const QVariantMap& preInitProperties,
                                       QString* errorMessage = nullptr) = 0;
    };
}

#define ScopeOneDriver_iid "org.scopeone.Driver/2.0"
Q_DECLARE_INTERFACE(scopeone::core::Driver, ScopeOneDriver_iid)

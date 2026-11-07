#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

#include "scopeone/Device.h"

namespace scopeone::core::internal
{
    // A DriverHost is a process of its own that runs hardware for ScopeOne, so a vendor library that
    // crashes or holds global state stays out of ScopeOne. The device these functions return
    // stands for what the DriverHost runs. It also implements CameraFrameSource.

    // A DriverHost that runs what the arguments name, such as one camera of a Micro-Manager
    // configuration for MicroManagerProvider
    DevicePtr startDriverHost(const QString& providerId,
                                        const QStringList& arguments,
                                        QString* errorMessage = nullptr);

    // One device of a ScopeOne driver, created by its name in the driver, the first one when the
    // name is empty. The provider id of the device is the label the configuration gives it.
    DevicePtr createScopeOneDevice(const QString& label,
                                   const QString& driverPath,
                                   const QString& deviceName,
                                   const QVariantMap& preInitProperties,
                                   QString* errorMessage = nullptr);
}

#pragma once

#include <QJsonObject>
#include <QString>

#include <QStringList>

#include "scopeone/HardwareProvider.h"

namespace scopeone::core::internal
{
    // A DriverHost is a process of its own that runs hardware for ScopeOne, so a vendor library that
    // crashes or holds global state stays out of ScopeOne. The provider these functions return
    // stands for what the DriverHost runs. It also implements CameraFrameSource.

    // A DriverHost that runs what the arguments name, such as one camera of a Micro-Manager
    // configuration for MicroManagerProvider
    HardwareProviderPtr startDriverHost(const QString& providerId,
                                        const QStringList& arguments,
                                        QString* errorMessage = nullptr);

    // The devices of one ScopeOne driver. Each driver is a provider of its own, as each camera of a
    // Micro-Manager configuration is a device of MicroManagerProvider.
    HardwareProviderPtr createScopeOneProvider(const QString& providerId,
                                               const QString& driverPath,
                                               const QJsonObject& options,
                                               QString* errorMessage = nullptr);
}

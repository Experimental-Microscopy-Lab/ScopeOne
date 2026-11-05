#pragma once

#include <QJsonObject>
#include <QString>

#include "scopeone/HardwareProvider.h"

namespace scopeone::core::internal
{
    HardwareProviderPtr createDriverHostProviderProxy(const QString& providerId,
                                                      const QString& driverPath,
                                                      const QJsonObject& options,
                                                      QString* errorMessage = nullptr);
}

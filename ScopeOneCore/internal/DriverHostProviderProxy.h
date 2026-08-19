#pragma once

#include <QJsonObject>
#include <QString>

#include "scopeone/HardwareProvider.h"

namespace scopeone::core::internal
{
    HardwareProviderPtr createDriverHostProviderProxy(const QString& providerId,
                                                      const QString& pluginPath,
                                                      const QJsonObject& options,
                                                      QString* errorMessage = nullptr);
}

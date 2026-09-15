#pragma once

#include <QJsonObject>
#include <QtPlugin>

#include "scopeone/HardwareProvider.h"

namespace scopeone::core
{
    class DriverHostProviderPlugin
    {
    public:
        virtual ~DriverHostProviderPlugin() = default;

        virtual QString providerId() const = 0;
        virtual HardwareProviderPtr createProvider(const QJsonObject& options,
                                                   QString* errorMessage = nullptr) = 0;
    };
}

#define ScopeOneDriverHostProviderPlugin_iid "org.scopeone.DriverHostProviderPlugin/1.0"
Q_DECLARE_INTERFACE(scopeone::core::DriverHostProviderPlugin,
                    ScopeOneDriverHostProviderPlugin_iid)

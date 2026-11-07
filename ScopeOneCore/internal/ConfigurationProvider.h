#pragma once

#include <QString>
#include <QStringList>

#include <vector>

#include "scopeone/ConfigSetting.h"

namespace scopeone::core::internal
{
    // A provider that defines configuration presets, which MicroManagerProvider reads from the
    // configuration file of Micro-Manager. Drivers do not implement it.
    class ConfigurationProvider
    {
    public:
        virtual ~ConfigurationProvider() = default;

        // The device properties that applying a preset sets. Empty when the provider cannot
        // tell, which is not an error.
        virtual std::vector<ConfigSetting> configSettings(const QString& groupName,
                                                          const QString& configName) const
        {
            (void)groupName;
            (void)configName;
            return {};
        }

        virtual QStringList availableConfigGroups() const = 0;
        virtual QStringList availableConfigs(const QString& groupName) const = 0;
        virtual QString currentConfig(const QString& groupName) const = 0;
        virtual bool setConfig(const QString& groupName,
                               const QString& configName,
                               QString* errorMessage = nullptr) = 0;
    };
}

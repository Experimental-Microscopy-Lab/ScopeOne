#pragma once

#include "scopeone/scopeone_core_export.h"

#include <QJsonObject>
#include <QString>

namespace scopeone::core
{
    enum class PluginKind
    {
        Processing,
        Tool,
        Hardware
    };

    struct PluginManifest
    {
        QString id;
        QString name;
        QString version;
        PluginKind kind{PluginKind::Processing};
        bool autoLoad{false};
        QJsonObject metadata;
    };

    inline constexpr int ScopeOnePluginApiVersion = 1;

    SCOPEONE_CORE_EXPORT QString pluginKindName(PluginKind kind);
    SCOPEONE_CORE_EXPORT bool parsePluginManifest(const QJsonObject& metadata,
                                                  PluginKind expectedKind,
                                                  PluginManifest& manifest,
                                                  QString* errorMessage = nullptr);
}

#pragma once

#include "scopeone/scopeone_core_export.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace scopeone::core
{
    // A library ScopeOne discovers and loads: a built-in processing plugin, or a driver that
    // ScopeOne_DriverHost loads for its device
    enum class PluginKind
    {
        Processing,
        Driver
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

    struct DiscoveredPlugin
    {
        PluginManifest manifest;
        PluginKind expectedKind{PluginKind::Processing};
        QString path;
        QString interfaceId;
        QJsonObject metadata;
        QString error;
    };

    inline constexpr int ScopeOneProcessingPluginApiVersion = 2;
    inline constexpr int ScopeOneDriverApiVersion = 1;

    SCOPEONE_CORE_EXPORT QString pluginKindName(PluginKind kind);
    SCOPEONE_CORE_EXPORT int pluginApiVersion(PluginKind kind);
    // The directory of the kind relative to the application or the user data directory
    SCOPEONE_CORE_EXPORT QString pluginDirectoryName(PluginKind kind);
    SCOPEONE_CORE_EXPORT QStringList pluginInterfaceIds(PluginKind kind);
    SCOPEONE_CORE_EXPORT QStringList pluginDirectories(PluginKind kind);
    SCOPEONE_CORE_EXPORT QString pluginSettingsKey(const QString& pluginId,
                                                  const QString& name);
    SCOPEONE_CORE_EXPORT bool pluginEnabled(const PluginManifest& manifest);
    SCOPEONE_CORE_EXPORT QList<DiscoveredPlugin> discoverPlugins(
        PluginKind kind,
        const QStringList& directories = {});
    SCOPEONE_CORE_EXPORT bool parsePluginManifest(const QJsonObject& metadata,
                                                  PluginKind expectedKind,
                                                  PluginManifest& manifest,
                                                  QString* errorMessage = nullptr);
}

#pragma once

#include "scopeone/scopeone_sdk_export.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

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
    inline constexpr int ScopeOneToolPluginApiVersion = 2;
    inline constexpr int ScopeOneHardwarePluginApiVersion = 1;

    SCOPEONE_SDK_EXPORT QString pluginKindName(PluginKind kind);
    SCOPEONE_SDK_EXPORT int pluginApiVersion(PluginKind kind);
    SCOPEONE_SDK_EXPORT QString pluginDirectoryName(PluginKind kind);
    SCOPEONE_SDK_EXPORT QStringList pluginInterfaceIds(PluginKind kind);
    SCOPEONE_SDK_EXPORT QStringList pluginDirectories(PluginKind kind);
    SCOPEONE_SDK_EXPORT QString pluginSettingsKey(const QString& pluginId,
                                                  const QString& name);
    SCOPEONE_SDK_EXPORT bool pluginEnabled(const PluginManifest& manifest);
    SCOPEONE_SDK_EXPORT QList<DiscoveredPlugin> discoverPlugins(
        PluginKind kind,
        const QStringList& directories = {});
    SCOPEONE_SDK_EXPORT bool parsePluginManifest(const QJsonObject& metadata,
                                                  PluginKind expectedKind,
                                                  PluginManifest& manifest,
                                                  QString* errorMessage = nullptr);
}

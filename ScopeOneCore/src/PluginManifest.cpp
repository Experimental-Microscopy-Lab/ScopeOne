#include "scopeone/PluginManifest.h"

#include "scopeone/DaqDevice.h"
#include "scopeone/DriverHostProviderPlugin.h"
#include "scopeone/ProcessingPlugin.h"
#include "scopeone/SignalSource.h"
#include "scopeone/ToolPlugin.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QPluginLoader>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

namespace scopeone::core
{
    QString pluginKindName(PluginKind kind)
    {
        switch (kind)
        {
        case PluginKind::Processing:
            return QStringLiteral("processing");
        case PluginKind::Tool:
            return QStringLiteral("tool");
        case PluginKind::Hardware:
            return QStringLiteral("hardware");
        }
        return {};
    }

    int pluginApiVersion(PluginKind kind)
    {
        switch (kind)
        {
        case PluginKind::Processing:
            return ScopeOneProcessingPluginApiVersion;
        case PluginKind::Tool:
            return ScopeOneToolPluginApiVersion;
        case PluginKind::Hardware:
            return ScopeOneHardwarePluginApiVersion;
        }
        return 0;
    }

    QString pluginDirectoryName(PluginKind kind)
    {
        return kind == PluginKind::Tool ? QStringLiteral("tools") : pluginKindName(kind);
    }

    QStringList pluginInterfaceIds(PluginKind kind)
    {
        switch (kind)
        {
        case PluginKind::Processing:
            return {QStringLiteral(ScopeOneProcessingPlugin_iid)};
        case PluginKind::Tool:
            return {QStringLiteral(ScopeOneToolPlugin_iid)};
        case PluginKind::Hardware:
            return {QStringLiteral(ScopeOneDriverHostProviderPlugin_iid),
                    QStringLiteral(SCOPEONE_DAQ_DEVICE_PLUGIN_IID),
                    QStringLiteral(SCOPEONE_SIGNAL_SOURCE_PLUGIN_IID)};
        }
        return {};
    }

    QStringList pluginDirectories(PluginKind kind)
    {
        const QString directoryName = pluginDirectoryName(kind);
        return {
            QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("plugins/%1").arg(directoryName)),
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                .filePath(QStringLiteral("plugins/%1").arg(directoryName))};
    }

    QString pluginSettingsKey(const QString& pluginId, const QString& name)
    {
        return QStringLiteral("Plugins/%1/%2").arg(pluginId, name);
    }

    bool pluginEnabled(const PluginManifest& manifest)
    {
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        const QString key = pluginSettingsKey(manifest.id, QStringLiteral("enabled"));
        return settings.contains(key) ? settings.value(key).toBool() : manifest.autoLoad;
    }

    QList<DiscoveredPlugin> discoverPlugins(PluginKind kind,
                                             const QStringList& directories)
    {
        QList<DiscoveredPlugin> plugins;
        const QStringList searchDirectories = directories.isEmpty()
                                                  ? pluginDirectories(kind)
                                                  : directories;
        for (const QString& directoryPath : searchDirectories)
        {
            const QDir directory(directoryPath);
            for (const QFileInfo& file : directory.entryInfoList(QDir::Files, QDir::Name))
            {
                if (!QLibrary::isLibrary(file.absoluteFilePath()))
                {
                    continue;
                }
                QPluginLoader loader(file.absoluteFilePath());
                const QJsonObject loaderMetadata = loader.metaData();
                const QString interfaceId = loaderMetadata.value(QStringLiteral("IID")).toString();
                if (interfaceId.isEmpty())
                {
                    continue;
                }

                DiscoveredPlugin plugin;
                plugin.expectedKind = kind;
                plugin.path = file.absoluteFilePath();
                plugin.interfaceId = interfaceId;
                plugin.metadata = loaderMetadata.value(QStringLiteral("MetaData")).toObject();
                parsePluginManifest(plugin.metadata, kind, plugin.manifest, &plugin.error);
                if (plugin.error.isEmpty() && !pluginInterfaceIds(kind).contains(interfaceId))
                {
                    plugin.error = QStringLiteral("plugin interface does not match its kind");
                }
                plugins.append(std::move(plugin));
            }
        }
        return plugins;
    }

    bool parsePluginManifest(const QJsonObject& metadata,
                             PluginKind expectedKind,
                             PluginManifest& manifest,
                             QString* errorMessage)
    {
        const QString id = metadata.value(QStringLiteral("id")).toString().trimmed();
        const QString name = metadata.value(QStringLiteral("name")).toString().trimmed();
        const QString version = metadata.value(QStringLiteral("version")).toString().trimmed();
        const QString kind = metadata.value(QStringLiteral("kind")).toString().trimmed();
        const int apiVersion = metadata.value(QStringLiteral("scopeOneApi")).toInt();
        const QString expectedKindName = pluginKindName(expectedKind);

        QString error;
        static const QRegularExpression idPattern(
            QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
        if (id.isEmpty() || name.isEmpty() || version.isEmpty())
        {
            error = QStringLiteral("plugin manifest requires id, name and version");
        }
        else if (!idPattern.match(id).hasMatch())
        {
            error = QStringLiteral("plugin id contains unsupported characters");
        }
        else if (kind != expectedKindName)
        {
            error = QStringLiteral("plugin kind must be '%1'").arg(expectedKindName);
        }
        else if (apiVersion != pluginApiVersion(expectedKind))
        {
            error = QStringLiteral("unsupported ScopeOne plugin API %1").arg(apiVersion);
        }

        if (!error.isEmpty())
        {
            if (errorMessage)
            {
                *errorMessage = error;
            }
            return false;
        }

        manifest.id = id;
        manifest.name = name;
        manifest.version = version;
        manifest.kind = expectedKind;
        manifest.autoLoad = metadata.value(QStringLiteral("autoLoad")).toBool();
        manifest.metadata = metadata;
        if (errorMessage)
        {
            errorMessage->clear();
        }
        return true;
    }
}

#include "scopeone/PluginManifest.h"

#include <QRegularExpression>

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
        else if (apiVersion != ScopeOnePluginApiVersion)
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

#include "PluginManagerDialog.h"

#include "scopeone/PluginManifest.h"
#include "scopeone/DaqDevice.h"
#include "scopeone/DriverHostProviderPlugin.h"
#include "scopeone/ProcessingPlugin.h"
#include "scopeone/SignalSource.h"
#include "scopeone/ScopeOneCore.h"
#include "scopeone/ToolPlugin.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLibrary>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPluginLoader>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>
#include <utility>

namespace scopeone::ui
{
    namespace
    {
        constexpr int kIdRole = Qt::UserRole;
        constexpr int kKindRole = Qt::UserRole + 1;
        constexpr int kStatusRole = Qt::UserRole + 2;
        constexpr int kMetadataRole = Qt::UserRole + 3;

        struct DiscoveredPlugin
        {
            scopeone::core::PluginManifest manifest;
            scopeone::core::PluginKind expectedKind{scopeone::core::PluginKind::Processing};
            QString path;
            QString interfaceId;
            QJsonObject metadata;
            QString error;
        };

        QStringList pluginInterfaceIds(scopeone::core::PluginKind kind);

        QList<DiscoveredPlugin> discoverPlugins()
        {
            using scopeone::core::PluginKind;
            const QList<std::pair<QString, PluginKind>> directories{
                {QStringLiteral("processing"), PluginKind::Processing},
                {QStringLiteral("tools"), PluginKind::Tool},
                {QStringLiteral("hardware"), PluginKind::Hardware}
            };
            QList<DiscoveredPlugin> plugins;
            const QStringList roots{
                QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("plugins")),
                QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                    .filePath(QStringLiteral("plugins"))
            };
            for (const QString& rootPath : roots)
            {
                const QDir root(rootPath);
                for (const auto& [directoryName, kind] : directories)
                {
                    const QDir directory(root.filePath(directoryName));
                    for (const QFileInfo& file : directory.entryInfoList(QDir::Files, QDir::Name))
                    {
                        if (!QLibrary::isLibrary(file.absoluteFilePath()))
                        {
                            continue;
                        }
                        QPluginLoader loader(file.absoluteFilePath());
                        DiscoveredPlugin plugin;
                        plugin.expectedKind = kind;
                        plugin.path = file.absoluteFilePath();
                        const QJsonObject loaderMetadata = loader.metaData();
                        plugin.interfaceId = loaderMetadata.value(QStringLiteral("IID")).toString();
                        plugin.metadata = loaderMetadata.value(QStringLiteral("MetaData")).toObject();
                        scopeone::core::parsePluginManifest(
                            plugin.metadata,
                            kind,
                            plugin.manifest,
                            &plugin.error);
                        if (plugin.error.isEmpty()
                            && !pluginInterfaceIds(kind).contains(
                                loaderMetadata.value(QStringLiteral("IID")).toString()))
                        {
                            plugin.error = QStringLiteral("plugin interface does not match its kind");
                        }
                        plugins.append(std::move(plugin));
                    }
                }
            }
            return plugins;
        }

        QString settingsKey(const QString& pluginId, const QString& name)
        {
            return QStringLiteral("Plugins/%1/%2").arg(pluginId, name);
        }

        QString pluginDirectoryName(scopeone::core::PluginKind kind)
        {
            return kind == scopeone::core::PluginKind::Tool
                       ? QStringLiteral("tools")
                       : scopeone::core::pluginKindName(kind);
        }

        QStringList pluginInterfaceIds(scopeone::core::PluginKind kind)
        {
            switch (kind)
            {
            case scopeone::core::PluginKind::Processing:
                return {QStringLiteral(ScopeOneProcessingPlugin_iid)};
            case scopeone::core::PluginKind::Tool:
                return {QStringLiteral(ScopeOneToolPlugin_iid)};
            case scopeone::core::PluginKind::Hardware:
                return {
                    QStringLiteral(ScopeOneDriverHostProviderPlugin_iid),
                    QStringLiteral(SCOPEONE_DAQ_DEVICE_PLUGIN_IID),
                    QStringLiteral(SCOPEONE_SIGNAL_SOURCE_PLUGIN_IID)};
            }
            return {};
        }
    }

    QStringList loadConfiguredHardwarePlugins(scopeone::core::ScopeOneCore& core)
    {
        QStringList errors;
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        for (const DiscoveredPlugin& plugin : discoverPlugins())
        {
            if (plugin.expectedKind != scopeone::core::PluginKind::Hardware)
            {
                continue;
            }
            if (plugin.interfaceId != QStringLiteral(ScopeOneDriverHostProviderPlugin_iid))
            {
                continue;
            }
            if (!plugin.error.isEmpty())
            {
                errors.append(QStringLiteral("%1: %2")
                                  .arg(QFileInfo(plugin.path).fileName(), plugin.error));
                continue;
            }

            const QString id = plugin.manifest.id;
            const QString enabledKey = settingsKey(id, QStringLiteral("enabled"));
            const bool enabled = settings.contains(enabledKey)
                                     ? settings.value(enabledKey).toBool()
                                     : plugin.manifest.autoLoad;
            if (!enabled)
            {
                continue;
            }

            const QString providerId = plugin.manifest.metadata
                                           .value(QStringLiteral("providerId"))
                                           .toString().trimmed();
            if (providerId != id)
            {
                errors.append(QStringLiteral("%1: providerId must match plugin id")
                                  .arg(QFileInfo(plugin.path).fileName()));
                continue;
            }
            const QVariantMap options = settings
                                            .value(settingsKey(id, QStringLiteral("options")))
                                            .toMap();
            QString error;
            if (!core.registerDriverHostProvider(providerId, plugin.path, options, &error))
            {
                errors.append(QStringLiteral("%1: %2")
                                  .arg(QFileInfo(plugin.path).fileName(), error));
            }
        }
        return errors;
    }

    PluginManagerDialog::PluginManagerDialog(QWidget* parent)
        : QDialog(parent)
    {
        setWindowTitle(tr("Plugin Manager"));
        resize(900, 600);

        auto* layout = new QVBoxLayout(this);
        m_table = new QTableWidget(this);
        m_table->setColumnCount(6);
        m_table->setHorizontalHeaderLabels(
            {tr("Enabled"), tr("Name"), tr("Type"), tr("Version"), tr("Status"), tr("Location")});
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->verticalHeader()->setVisible(false);
        m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
        layout->addWidget(m_table, 1);

        layout->addWidget(new QLabel(tr("Plugin metadata and diagnostics"), this));
        m_detailsEdit = new QPlainTextEdit(this);
        m_detailsEdit->setReadOnly(true);
        m_detailsEdit->setMaximumHeight(160);
        layout->addWidget(m_detailsEdit);

        layout->addWidget(new QLabel(tr("Hardware options (JSON)"), this));
        m_optionsEdit = new QPlainTextEdit(this);
        m_optionsEdit->setMaximumHeight(100);
        m_optionsEdit->setPlaceholderText(QStringLiteral("{}"));
        layout->addWidget(m_optionsEdit);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close,
                                             this);
        auto* installButton = buttons->addButton(tr("Install..."), QDialogButtonBox::ActionRole);
        layout->addWidget(buttons);
        connect(buttons->button(QDialogButtonBox::Save), &QPushButton::clicked,
                this, &PluginManagerDialog::saveHardwareSettings);
        connect(installButton, &QPushButton::clicked,
                this, &PluginManagerDialog::installPlugin);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_table, &QTableWidget::itemSelectionChanged,
                this, &PluginManagerDialog::showSelectedOptions);
        refreshPlugins();
    }

    void PluginManagerDialog::refreshPlugins()
    {
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        const QList<DiscoveredPlugin> plugins = discoverPlugins();
        m_table->setRowCount(plugins.size());
        for (int row = 0; row < plugins.size(); ++row)
        {
            const DiscoveredPlugin& plugin = plugins.at(row);
            const bool hardware = plugin.expectedKind == scopeone::core::PluginKind::Hardware;
            auto* enabled = new QTableWidgetItem();
            enabled->setData(kIdRole, plugin.manifest.id);
            enabled->setData(kKindRole, static_cast<int>(plugin.expectedKind));
            const QString status = plugin.error.isEmpty() ? tr("Ready") : plugin.error;
            enabled->setData(kStatusRole, status);
            enabled->setData(
                kMetadataRole,
                QString::fromUtf8(QJsonDocument(plugin.metadata).toJson(QJsonDocument::Indented)));
            if (hardware && plugin.error.isEmpty())
            {
                enabled->setFlags(enabled->flags() | Qt::ItemIsUserCheckable);
                const QString key = settingsKey(plugin.manifest.id, QStringLiteral("enabled"));
                const bool checked = settings.contains(key)
                                         ? settings.value(key).toBool()
                                         : plugin.manifest.autoLoad;
                enabled->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            }
            m_table->setItem(row, 0, enabled);
            m_table->setItem(row, 1, new QTableWidgetItem(
                                 plugin.manifest.name.isEmpty()
                                     ? QFileInfo(plugin.path).completeBaseName()
                                     : plugin.manifest.name));
            m_table->setItem(row, 2, new QTableWidgetItem(
                                 scopeone::core::pluginKindName(plugin.expectedKind)));
            m_table->setItem(row, 3, new QTableWidgetItem(plugin.manifest.version));
            m_table->setItem(row, 4, new QTableWidgetItem(
                                 status));
            m_table->setItem(row, 5, new QTableWidgetItem(plugin.path));
        }
        if (m_table->rowCount() > 0)
        {
            m_table->selectRow(0);
        }
    }

    void PluginManagerDialog::showSelectedOptions()
    {
        const int row = m_table->currentRow();
        if (row < 0)
        {
            m_optionsEdit->clear();
            m_optionsEdit->setEnabled(false);
            return;
        }
        const QTableWidgetItem* item = m_table->item(row, 0);
        const auto kind = static_cast<scopeone::core::PluginKind>(item->data(kKindRole).toInt());
        const QString id = item->data(kIdRole).toString();
        const QString status = item->data(kStatusRole).toString();
        m_detailsEdit->setPlainText(
            item->data(kMetadataRole).toString()
            + QStringLiteral("\n\n")
            + tr("Status: %1").arg(status));
        const bool hardware = kind == scopeone::core::PluginKind::Hardware
                              && !id.isEmpty()
                              && status == tr("Ready");
        m_optionsEdit->setEnabled(hardware);
        if (!hardware)
        {
            m_optionsEdit->clear();
            return;
        }
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        const QJsonObject options = QJsonObject::fromVariantMap(
            settings.value(settingsKey(id, QStringLiteral("options"))).toMap());
        m_optionsEdit->setPlainText(QString::fromUtf8(
            QJsonDocument(options).toJson(QJsonDocument::Indented)));
    }

    void PluginManagerDialog::installPlugin()
    {
        const QString sourcePath = QFileDialog::getOpenFileName(
            this, tr("Install Plugin"), {}, tr("Plugin libraries (*.dll *.so *.dylib)"));
        if (sourcePath.isEmpty())
        {
            return;
        }

        QPluginLoader loader(sourcePath);
        const QJsonObject loaderMetadata = loader.metaData();
        const QJsonObject metadata = loaderMetadata.value(QStringLiteral("MetaData")).toObject();
        scopeone::core::PluginManifest manifest;
        QString error;
        bool valid = false;
        scopeone::core::PluginKind kind = scopeone::core::PluginKind::Processing;
        for (const auto candidate : {scopeone::core::PluginKind::Processing,
                                     scopeone::core::PluginKind::Tool,
                                     scopeone::core::PluginKind::Hardware})
        {
            if (scopeone::core::parsePluginManifest(metadata, candidate, manifest, &error))
            {
                kind = candidate;
                valid = true;
                break;
            }
        }
        if (!valid)
        {
            QMessageBox::warning(this, tr("Plugin Manager"), error);
            return;
        }
        if (!pluginInterfaceIds(kind).contains(loaderMetadata.value(QStringLiteral("IID")).toString()))
        {
            QMessageBox::warning(this, tr("Plugin Manager"),
                                 tr("The plugin interface does not match its declared type."));
            return;
        }

        QDir userRoot(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
        const QString pluginDirectory = userRoot.filePath(
            QStringLiteral("plugins/%1").arg(pluginDirectoryName(kind)));
        if (!QDir().mkpath(pluginDirectory))
        {
            QMessageBox::warning(this, tr("Plugin Manager"), tr("The plugin directory could not be created."));
            return;
        }
        const QString destination = QDir(pluginDirectory).filePath(QFileInfo(sourcePath).fileName());
        if (QFileInfo(sourcePath).canonicalFilePath() == QFileInfo(destination).canonicalFilePath())
        {
            QMessageBox::information(this, tr("Plugin Manager"), tr("This plugin is already installed."));
            return;
        }
        if (QFileInfo::exists(destination) && !QFile::remove(destination))
        {
            QMessageBox::warning(this, tr("Plugin Manager"), tr("The existing plugin could not be replaced."));
            return;
        }
        if (!QFile::copy(sourcePath, destination))
        {
            QMessageBox::warning(this, tr("Plugin Manager"), tr("The plugin could not be installed."));
            return;
        }
        refreshPlugins();
        QMessageBox::information(this, tr("Plugin Manager"),
                                 tr("The plugin will be available after ScopeOne restarts."));
    }

    void PluginManagerDialog::saveHardwareSettings()
    {
        const int row = m_table->currentRow();
        if (row < 0)
        {
            return;
        }
        QTableWidgetItem* item = m_table->item(row, 0);
        const auto kind = static_cast<scopeone::core::PluginKind>(item->data(kKindRole).toInt());
        const QString id = item->data(kIdRole).toString();
        QVariantMap selectedOptions;
        if (kind == scopeone::core::PluginKind::Hardware && !id.isEmpty())
        {
            QJsonParseError parseError;
            QByteArray optionJson = m_optionsEdit->toPlainText().trimmed().toUtf8();
            if (optionJson.isEmpty())
            {
                optionJson = QByteArrayLiteral("{}");
            }
            const QJsonDocument options = QJsonDocument::fromJson(optionJson, &parseError);
            if (parseError.error != QJsonParseError::NoError || !options.isObject())
            {
                QMessageBox::warning(this, tr("Plugin Manager"), tr("Hardware options must be a JSON object."));
                return;
            }
            selectedOptions = options.object().toVariantMap();
        }

        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        for (int pluginRow = 0; pluginRow < m_table->rowCount(); ++pluginRow)
        {
            QTableWidgetItem* pluginItem = m_table->item(pluginRow, 0);
            const auto pluginKind = static_cast<scopeone::core::PluginKind>(
                pluginItem->data(kKindRole).toInt());
            const QString pluginId = pluginItem->data(kIdRole).toString();
            if (pluginKind == scopeone::core::PluginKind::Hardware && !pluginId.isEmpty())
            {
                settings.setValue(settingsKey(pluginId, QStringLiteral("enabled")),
                                  pluginItem->checkState() == Qt::Checked);
            }
        }
        if (kind == scopeone::core::PluginKind::Hardware && !id.isEmpty())
        {
            settings.setValue(settingsKey(id, QStringLiteral("options")), selectedOptions);
        }
        QMessageBox::information(this, tr("Plugin Manager"),
                                 tr("Plugin settings will take effect after ScopeOne restarts."));
    }
}

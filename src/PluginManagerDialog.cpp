#include "PluginManagerDialog.h"

#include "scopeone/PluginManifest.h"
#include "scopeone/DaqDevice.h"
#include "scopeone/DriverHostProviderPlugin.h"
#include "scopeone/ProcessingPlugin.h"
#include "scopeone/SignalSource.h"
#include "scopeone/ScopeOneCore.h"
#include "scopeone/ToolPlugin.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
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

        using scopeone::core::DiscoveredPlugin;

        QList<DiscoveredPlugin> discoverPlugins()
        {
            QList<DiscoveredPlugin> plugins = scopeone::core::discoverPlugins(
                scopeone::core::PluginKind::Processing);
            for (const scopeone::core::PluginKind kind :
                 {scopeone::core::PluginKind::Tool, scopeone::core::PluginKind::Hardware})
            {
                plugins.append(scopeone::core::discoverPlugins(kind));
            }
            return plugins;
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
            if (!scopeone::core::pluginEnabled(plugin.manifest))
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
                                            .value(scopeone::core::pluginSettingsKey(id, QStringLiteral("options")))
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
                const QString key = scopeone::core::pluginSettingsKey(
                    plugin.manifest.id, QStringLiteral("enabled"));
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
            settings.value(scopeone::core::pluginSettingsKey(
                               id, QStringLiteral("options"))).toMap());
        m_optionsEdit->setPlainText(QString::fromUtf8(
            QJsonDocument(options).toJson(QJsonDocument::Indented)));
    }

    void PluginManagerDialog::installPlugin()
    {
        const QString sourceDirectory = QFileDialog::getExistingDirectory(
            this, tr("Install Plugin Package"));
        if (sourceDirectory.isEmpty())
        {
            return;
        }

        QList<scopeone::core::DiscoveredPlugin> candidates;
        scopeone::core::PluginKind kind = scopeone::core::PluginKind::Processing;
        for (const auto candidateKind : {scopeone::core::PluginKind::Processing,
                                         scopeone::core::PluginKind::Tool,
                                         scopeone::core::PluginKind::Hardware})
        {
            for (const auto& candidate :
                 scopeone::core::discoverPlugins(candidateKind, {sourceDirectory}))
            {
                if (candidate.error.isEmpty())
                {
                    candidates.append(candidate);
                    kind = candidateKind;
                }
            }
        }
        if (candidates.size() != 1)
        {
            QMessageBox::warning(this, tr("Plugin Manager"),
                                 tr("A plugin package must contain one plugin library."));
            return;
        }

        QDir userRoot(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
        const QString pluginDirectory = userRoot.filePath(
            QStringLiteral("plugins/%1").arg(scopeone::core::pluginDirectoryName(kind)));
        if (!QDir().mkpath(pluginDirectory))
        {
            QMessageBox::warning(this, tr("Plugin Manager"), tr("The plugin directory could not be created."));
            return;
        }
        if (QFileInfo(sourceDirectory).canonicalFilePath()
            == QFileInfo(pluginDirectory).canonicalFilePath())
        {
            QMessageBox::information(this, tr("Plugin Manager"), tr("This plugin is already installed."));
            return;
        }
        for (const QFileInfo& source : QDir(sourceDirectory).entryInfoList(QDir::Files))
        {
            const QString destination = QDir(pluginDirectory).filePath(source.fileName());
            QFile::remove(destination);
            if (!QFile::copy(source.absoluteFilePath(), destination))
            {
                QMessageBox::warning(this, tr("Plugin Manager"),
                                     tr("The plugin package could not be installed."));
                return;
            }
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
                settings.setValue(scopeone::core::pluginSettingsKey(
                                      pluginId, QStringLiteral("enabled")),
                                  pluginItem->checkState() == Qt::Checked);
            }
        }
        if (kind == scopeone::core::PluginKind::Hardware && !id.isEmpty())
        {
            settings.setValue(scopeone::core::pluginSettingsKey(
                                  id, QStringLiteral("options")), selectedOptions);
        }
        QMessageBox::information(this, tr("Plugin Manager"),
                                 tr("Plugin settings will take effect after ScopeOne restarts."));
    }
}

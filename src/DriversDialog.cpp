#include "DriversDialog.h"

#include "scopeone/DriverManifest.h"
#include "scopeone/ScopeOneCore.h"

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
        constexpr int kStatusRole = Qt::UserRole + 1;
        constexpr int kMetadataRole = Qt::UserRole + 2;

        using scopeone::core::DiscoveredDriver;
        using scopeone::core::discoverDrivers;
        using scopeone::core::driverSettingsKey;
    }

    QStringList loadConfiguredDrivers(scopeone::core::ScopeOneCore& core)
    {
        QStringList errors;
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        for (const DiscoveredDriver& driver : discoverDrivers())
        {
            if (!driver.error.isEmpty())
            {
                errors.append(QStringLiteral("%1: %2")
                                  .arg(QFileInfo(driver.path).fileName(), driver.error));
                continue;
            }

            const QString id = driver.manifest.id;
            if (!scopeone::core::driverEnabled(driver.manifest))
            {
                continue;
            }

            const QString providerId = driver.manifest.metadata
                                           .value(QStringLiteral("providerId"))
                                           .toString().trimmed();
            if (providerId != id)
            {
                errors.append(QStringLiteral("%1: providerId must match the driver id")
                                  .arg(QFileInfo(driver.path).fileName()));
                continue;
            }
            const QVariantMap options = settings
                                            .value(driverSettingsKey(id, QStringLiteral("options")))
                                            .toMap();
            QString error;
            if (!core.registerDriverHostProvider(providerId, driver.path, options, &error))
            {
                errors.append(QStringLiteral("%1: %2")
                                  .arg(QFileInfo(driver.path).fileName(), error));
            }
        }
        return errors;
    }

    DriversDialog::DriversDialog(QWidget* parent)
        : QDialog(parent)
    {
        setWindowTitle(tr("Drivers"));
        resize(900, 600);

        auto* layout = new QVBoxLayout(this);
        m_table = new QTableWidget(this);
        m_table->setColumnCount(5);
        m_table->setHorizontalHeaderLabels(
            {tr("Enabled"), tr("Name"), tr("Version"), tr("Status"), tr("Location")});
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->verticalHeader()->setVisible(false);
        m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        layout->addWidget(m_table, 1);

        layout->addWidget(new QLabel(tr("Metadata and diagnostics"), this));
        m_detailsEdit = new QPlainTextEdit(this);
        m_detailsEdit->setReadOnly(true);
        m_detailsEdit->setMaximumHeight(160);
        layout->addWidget(m_detailsEdit);

        layout->addWidget(new QLabel(tr("Driver options (JSON)"), this));
        m_optionsEdit = new QPlainTextEdit(this);
        m_optionsEdit->setMaximumHeight(100);
        m_optionsEdit->setPlaceholderText(QStringLiteral("{}"));
        layout->addWidget(m_optionsEdit);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close,
                                             this);
        auto* installButton = buttons->addButton(tr("Install Driver..."), QDialogButtonBox::ActionRole);
        layout->addWidget(buttons);
        connect(buttons->button(QDialogButtonBox::Save), &QPushButton::clicked,
                this, &DriversDialog::saveSettings);
        connect(installButton, &QPushButton::clicked,
                this, &DriversDialog::installDriver);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_table, &QTableWidget::itemSelectionChanged,
                this, &DriversDialog::showSelectedOptions);
        refreshDrivers();
    }

    void DriversDialog::refreshDrivers()
    {
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        const QList<DiscoveredDriver> drivers = discoverDrivers();
        m_table->setRowCount(drivers.size());
        for (int row = 0; row < drivers.size(); ++row)
        {
            const DiscoveredDriver& driver = drivers.at(row);
            auto* enabled = new QTableWidgetItem();
            enabled->setData(kIdRole, driver.manifest.id);
            const QString status = driver.error.isEmpty() ? tr("Ready") : driver.error;
            enabled->setData(kStatusRole, status);
            enabled->setData(
                kMetadataRole,
                QString::fromUtf8(QJsonDocument(driver.metadata).toJson(QJsonDocument::Indented)));
            if (driver.error.isEmpty())
            {
                enabled->setFlags(enabled->flags() | Qt::ItemIsUserCheckable);
                const QString key = driverSettingsKey(
                    driver.manifest.id, QStringLiteral("enabled"));
                const bool checked = settings.contains(key)
                                         ? settings.value(key).toBool()
                                         : driver.manifest.autoLoad;
                enabled->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            }
            m_table->setItem(row, 0, enabled);
            m_table->setItem(row, 1, new QTableWidgetItem(
                                 driver.manifest.name.isEmpty()
                                     ? QFileInfo(driver.path).completeBaseName()
                                     : driver.manifest.name));
            m_table->setItem(row, 2, new QTableWidgetItem(driver.manifest.version));
            m_table->setItem(row, 3, new QTableWidgetItem(status));
            m_table->setItem(row, 4, new QTableWidgetItem(driver.path));
        }
        if (m_table->rowCount() > 0)
        {
            m_table->selectRow(0);
        }
    }

    void DriversDialog::showSelectedOptions()
    {
        const int row = m_table->currentRow();
        if (row < 0)
        {
            m_optionsEdit->clear();
            m_optionsEdit->setEnabled(false);
            return;
        }
        const QTableWidgetItem* item = m_table->item(row, 0);
        const QString id = item->data(kIdRole).toString();
        const QString status = item->data(kStatusRole).toString();
        m_detailsEdit->setPlainText(
            item->data(kMetadataRole).toString()
            + QStringLiteral("\n\n")
            + tr("Status: %1").arg(status));
        const bool driver = !id.isEmpty() && status == tr("Ready");
        m_optionsEdit->setEnabled(driver);
        if (!driver)
        {
            m_optionsEdit->clear();
            return;
        }
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        const QJsonObject options = QJsonObject::fromVariantMap(
            settings.value(driverSettingsKey(
                               id, QStringLiteral("options"))).toMap());
        m_optionsEdit->setPlainText(QString::fromUtf8(
            QJsonDocument(options).toJson(QJsonDocument::Indented)));
    }

    void DriversDialog::installDriver()
    {
        const QString sourceDirectory = QFileDialog::getExistingDirectory(
            this, tr("Install Driver"));
        if (sourceDirectory.isEmpty())
        {
            return;
        }

        QList<DiscoveredDriver> candidates;
        for (const auto& candidate : discoverDrivers({sourceDirectory}))
        {
            if (candidate.error.isEmpty())
            {
                candidates.append(candidate);
            }
        }
        if (candidates.size() != 1)
        {
            QMessageBox::warning(this, tr("Drivers"),
                                 tr("The folder must contain one driver library."));
            return;
        }

        QDir userRoot(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
        const QString driverDirectory = userRoot.filePath(QStringLiteral("drivers"));
        if (!QDir().mkpath(driverDirectory))
        {
            QMessageBox::warning(this, tr("Drivers"), tr("The install directory could not be created."));
            return;
        }
        if (QFileInfo(sourceDirectory).canonicalFilePath()
            == QFileInfo(driverDirectory).canonicalFilePath())
        {
            QMessageBox::information(this, tr("Drivers"), tr("This is already installed."));
            return;
        }
        for (const QFileInfo& source : QDir(sourceDirectory).entryInfoList(QDir::Files))
        {
            const QString destination = QDir(driverDirectory).filePath(source.fileName());
            QFile::remove(destination);
            if (!QFile::copy(source.absoluteFilePath(), destination))
            {
                QMessageBox::warning(this, tr("Drivers"),
                                     tr("The files could not be installed."));
                return;
            }
        }
        refreshDrivers();
        QMessageBox::information(this, tr("Drivers"),
                                 tr("It will be available after ScopeOne restarts."));
    }

    void DriversDialog::saveSettings()
    {
        const int row = m_table->currentRow();
        if (row < 0)
        {
            return;
        }
        QTableWidgetItem* item = m_table->item(row, 0);
        const QString id = item->data(kIdRole).toString();
        QVariantMap selectedOptions;
        if (!id.isEmpty())
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
                QMessageBox::warning(this, tr("Drivers"), tr("Driver options must be a JSON object."));
                return;
            }
            selectedOptions = options.object().toVariantMap();
        }

        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        for (int driverRow = 0; driverRow < m_table->rowCount(); ++driverRow)
        {
            QTableWidgetItem* driverItem = m_table->item(driverRow, 0);
            const QString driverId = driverItem->data(kIdRole).toString();
            if (!driverId.isEmpty())
            {
                settings.setValue(driverSettingsKey(
                                      driverId, QStringLiteral("enabled")),
                                  driverItem->checkState() == Qt::Checked);
            }
        }
        if (!id.isEmpty())
        {
            settings.setValue(driverSettingsKey(
                                  id, QStringLiteral("options")), selectedOptions);
        }
        QMessageBox::information(this, tr("Drivers"),
                                 tr("The settings will take effect after ScopeOne restarts."));
    }
}

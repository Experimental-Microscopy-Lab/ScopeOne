#include "DriversDialog.h"

#include "scopeone/DriverManifest.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace scopeone::ui
{
    namespace
    {
        constexpr int kStatusRole = Qt::UserRole;
        constexpr int kMetadataRole = Qt::UserRole + 1;

        using scopeone::core::DiscoveredDriver;
        using scopeone::core::discoverDrivers;

        QString userDriverDirectory()
        {
            return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                .filePath(QStringLiteral("drivers"));
        }

        // A file already in the drivers folder is kept, as removing it would remove the source too
        bool copyFiles(const QFileInfoList& files, const QString& driverDirectory)
        {
            for (const QFileInfo& source : files)
            {
                const QString destination = QDir(driverDirectory).filePath(source.fileName());
                if (QFileInfo(destination).canonicalFilePath() == source.canonicalFilePath())
                {
                    continue;
                }
                QFile::remove(destination);
                if (!QFile::copy(source.absoluteFilePath(), destination))
                {
                    return false;
                }
            }
            return true;
        }
    }

    bool DriversDialog::makeDriverDirectory(const QString& path)
    {
        if (QDir().mkpath(path))
        {
            return true;
        }
        QMessageBox::warning(this, tr("Drivers"), tr("The install directory could not be created."));
        return false;
    }

    DriversDialog::DriversDialog(QWidget* parent)
        : QDialog(parent)
    {
        setWindowTitle(tr("Drivers"));
        resize(900, 600);

        auto* layout = new QVBoxLayout(this);
        m_table = new QTableWidget(this);
        m_table->setColumnCount(4);
        m_table->setHorizontalHeaderLabels(
            {tr("Name"), tr("Version"), tr("Status"), tr("Location")});
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->verticalHeader()->setVisible(false);
        m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        layout->addWidget(m_table, 1);

        layout->addWidget(new QLabel(tr("Metadata and diagnostics"), this));
        m_detailsEdit = new QPlainTextEdit(this);
        m_detailsEdit->setReadOnly(true);
        m_detailsEdit->setMaximumHeight(160);
        layout->addWidget(m_detailsEdit);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        auto* installButton = buttons->addButton(tr("Install Driver..."), QDialogButtonBox::ActionRole);
        auto* descriptionButton = buttons->addButton(tr("Install Device Description..."), QDialogButtonBox::ActionRole);
        layout->addWidget(buttons);
        connect(installButton, &QPushButton::clicked,
                this, &DriversDialog::installDriver);
        connect(descriptionButton, &QPushButton::clicked,
                this, &DriversDialog::installDescription);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_table, &QTableWidget::itemSelectionChanged,
                this, &DriversDialog::showSelectedDetails);
        refreshDrivers();
    }

    void DriversDialog::refreshDrivers()
    {
        const QList<DiscoveredDriver> drivers = discoverDrivers();
        m_table->setRowCount(drivers.size());
        for (int row = 0; row < drivers.size(); ++row)
        {
            const DiscoveredDriver& driver = drivers.at(row);
            const QString status = driver.error.isEmpty() ? tr("Ready") : driver.error;
            auto* name = new QTableWidgetItem(driver.manifest.name.isEmpty()
                                                  ? QFileInfo(driver.path).completeBaseName()
                                                  : driver.manifest.name);
            name->setData(kStatusRole, status);
            name->setData(
                kMetadataRole,
                QString::fromUtf8(QJsonDocument(driver.metadata).toJson(QJsonDocument::Indented)));
            m_table->setItem(row, 0, name);
            m_table->setItem(row, 1, new QTableWidgetItem(driver.manifest.version));
            m_table->setItem(row, 2, new QTableWidgetItem(status));
            m_table->setItem(row, 3, new QTableWidgetItem(driver.path));
        }
        if (m_table->rowCount() > 0)
        {
            m_table->selectRow(0);
        }
    }

    void DriversDialog::showSelectedDetails()
    {
        const int row = m_table->currentRow();
        if (row < 0)
        {
            m_detailsEdit->clear();
            return;
        }
        const QTableWidgetItem* item = m_table->item(row, 0);
        m_detailsEdit->setPlainText(
            item->data(kMetadataRole).toString()
            + QStringLiteral("\n\n")
            + tr("Status: %1").arg(item->data(kStatusRole).toString()));
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

        const QString driverDirectory = userDriverDirectory();
        if (!makeDriverDirectory(driverDirectory))
        {
            return;
        }
        if (QFileInfo(sourceDirectory).canonicalFilePath()
            == QFileInfo(driverDirectory).canonicalFilePath())
        {
            QMessageBox::information(this, tr("Drivers"), tr("This is already installed."));
            return;
        }
        if (!copyFiles(QDir(sourceDirectory).entryInfoList(QDir::Files), driverDirectory))
        {
            QMessageBox::warning(this, tr("Drivers"),
                                 tr("The files could not be installed."));
            return;
        }
        refreshDrivers();
        QMessageBox::information(this, tr("Drivers"),
                                 tr("The driver is installed."));
    }

    void DriversDialog::installDescription()
    {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Install Device Description"), QString(), tr("Device description (*.json)"));
        if (path.isEmpty())
        {
            return;
        }

        const QFileInfo source(path);
        const QList<DiscoveredDriver> found = discoverDrivers({source.absolutePath()});
        const bool isDescription = std::any_of(found.cbegin(), found.cend(),
            [&](const DiscoveredDriver& driver)
            { return driver.description && driver.path == source.absoluteFilePath(); });
        if (!isDescription)
        {
            QMessageBox::warning(this, tr("Drivers"),
                                 tr("The file is not a device description."));
            return;
        }

        const QString driverDirectory = userDriverDirectory();
        if (!makeDriverDirectory(driverDirectory))
        {
            return;
        }
        if (!copyFiles({source}, driverDirectory))
        {
            QMessageBox::warning(this, tr("Drivers"),
                                 tr("The files could not be installed."));
            return;
        }
        refreshDrivers();
        QMessageBox::information(this, tr("Drivers"),
                                 tr("The device description is installed."));
    }
}

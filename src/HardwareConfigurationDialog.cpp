#include "HardwareConfigurationDialog.h"
#include "LightSourcesPage.h"
#include "scopeone/DriverManifest.h"
#include "scopeone/ScopeOneCore.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTreeWidget>

namespace scopeone::ui
{
    using namespace scopeone::core;
    namespace
    {
        bool deviceDialog(QWidget* parent, HardwareConfiguration::ScopeOneDevice& device, bool adding)
        {
            const auto drivers = discoverDrivers();
            QDialog dialog(parent);
            dialog.setWindowTitle(adding ? QObject::tr("Add Device") : QObject::tr("Edit Device"));
            auto* layout = new QVBoxLayout(&dialog);
            auto* form = new QFormLayout;
            layout->addLayout(form);
            auto* driver = new QComboBox(&dialog);
            for (const auto& d : drivers)
                if (d.error.isEmpty() && d.manifest.id != QStringLiteral("scopeone.serial"))
                    driver->addItem(d.manifest.name, d.manifest.id);
            auto* label = new QLineEdit(device.label, &dialog);
            auto* name = new QComboBox(&dialog);
            name->setEditable(true);
            form->addRow(QObject::tr("Driver"), driver);
            form->addRow(QObject::tr("Label"), label);
            form->addRow(QObject::tr("Device"), name);
            auto* properties = new QFormLayout;
            layout->addLayout(properties);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            layout->addWidget(buttons);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            auto* process = new QProcess(&dialog);
            buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
            QHash<QString, QComboBox*> editors;
            bool pendingDescription = false;
            auto describe = [&]()
            {
                buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
                if (process->state() != QProcess::NotRunning) { pendingDescription = true; return; }
                pendingDescription = false;
                const QString id = driver->currentData().toString();
                const DiscoveredDriver* selected = nullptr;
                const DiscoveredDriver* serial = nullptr;
                for (const auto& d : drivers) { if (d.manifest.id == id) selected = &d; if (d.manifest.id == QStringLiteral("scopeone.serial")) serial = &d; }
                if (!selected) return;
                const QString path = selected->description && serial ? serial->path : selected->path;
                process->start(QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
                    QStringLiteral("ScopeOne_DriverHost.exe")
#else
                    QStringLiteral("ScopeOne_DriverHost")
#endif
                    ), {QStringLiteral("--describe"), QStringLiteral("--driver"), path,
                        QStringLiteral("--device"), name->currentText()});
            };
            QObject::connect(process, &QProcess::finished, &dialog, [&](int code, QProcess::ExitStatus)
            {
                if (pendingDescription) { process->readAllStandardOutput(); process->readAllStandardError(); describe(); return; }
                if (code != 0) { QMessageBox::warning(&dialog, QObject::tr("Driver"), QString::fromUtf8(process->readAllStandardError())); return; }
                const QJsonObject result = QJsonDocument::fromJson(process->readAllStandardOutput()).object();
                const QString id = driver->currentData().toString();
                QJsonObject metadata;
                for (const auto& d : drivers) if (d.manifest.id == id) metadata = d.metadata;
                const QString previousName = name->currentText();
                QSignalBlocker blocker(name);
                name->clear();
                QJsonArray names = metadata.value(QStringLiteral("devices")).toArray();
                if (names.isEmpty()) names = result.value(QStringLiteral("devices")).toArray();
                for (const auto& n : names) name->addItem(n.toString());
                if (!previousName.isEmpty()) name->setCurrentText(previousName);
                // The first query lists devices; its empty name does not describe the selected device.
                if (previousName.isEmpty() && !name->currentText().isEmpty()) { describe(); return; }
                while (QLayoutItem* item = properties->takeAt(0)) { delete item->widget(); delete item; }
                editors.clear();
                for (const auto& value : result.value(QStringLiteral("properties")).toArray())
                {
                    const QJsonObject p = value.toObject();
                    const QString key = p.value(QStringLiteral("name")).toString();
                    auto* editor = new QComboBox(&dialog);
                    editor->setEditable(true);
                    for (const auto& v : p.value(QStringLiteral("allowedValues")).toArray()) editor->addItem(v.toString());
                    QString initial = p.value(QStringLiteral("defaultValue")).toString();
                    if (key == QStringLiteral("BaudRate") && metadata.contains(QStringLiteral("serial"))) initial = QString::number(metadata.value(QStringLiteral("serial")).toObject().value(key).toInt(9600));
                    if (key == QStringLiteral("AnswerTimeout") && metadata.contains(key)) initial = QString::number(metadata.value(key).toInt());
                    for (const auto& old : device.properties) if (old.first == key) initial = old.second;
                    editor->setCurrentText(initial);
                    editors.insert(key, editor);
                    properties->addRow(key, editor);
                }
                buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
            });
            QObject::connect(process, &QProcess::errorOccurred, &dialog, [&](QProcess::ProcessError error)
            {
                if (error == QProcess::FailedToStart)
                    QMessageBox::warning(&dialog, QObject::tr("Driver"), process->errorString());
            });
            QObject::connect(driver, &QComboBox::currentIndexChanged, &dialog, [&](int) { name->clear(); describe(); });
            QObject::connect(name, &QComboBox::activated, &dialog, [&](int) { describe(); });
            QObject::connect(name->lineEdit(), &QLineEdit::editingFinished, &dialog, [&]() { describe(); });
            QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]()
            {
                const auto valid = [](const QString& value) { return !value.contains(QLatin1Char(',')) && !value.contains(QLatin1Char('\n')) && !value.contains(QLatin1Char('\r')); };
                if (driver->currentIndex() < 0 || label->text().trimmed().isEmpty() || name->currentText().trimmed().isEmpty()
                    || !valid(label->text()) || !valid(name->currentText()))
                { QMessageBox::warning(&dialog, QObject::tr("Device"), QObject::tr("Enter a driver, label and device name without commas or line breaks.")); return; }
                for (auto it = editors.cbegin(); it != editors.cend(); ++it)
                    if (!valid(it.value()->currentText()))
                    { QMessageBox::warning(&dialog, QObject::tr("Device"), QObject::tr("Property values cannot contain commas or line breaks.")); return; }
                dialog.accept();
            });
            { QSignalBlocker blocker(driver); driver->setCurrentIndex(qMax(0, driver->findData(device.driverId))); }
            driver->setEnabled(adding);
            label->setReadOnly(!adding);
            name->setEnabled(adding);
            name->setCurrentText(device.deviceName);
            describe();
            if (dialog.exec() != QDialog::Accepted || label->text().trimmed().isEmpty()) return false;
            device.label = label->text().trimmed();
            device.driverId = driver->currentData().toString();
            device.deviceName = name->currentText().trimmed();
            for (auto it = editors.cbegin(); it != editors.cend(); ++it)
            {
                bool found = false;
                for (auto& property : device.properties) if (property.first == it.key()) { property.second = it.value()->currentText(); found = true; }
                if (!found) device.properties.append({it.key(), it.value()->currentText()});
            }
            return true;
        }
    }

    HardwareConfigurationDialog::HardwareConfigurationDialog(ScopeOneCore* core, QWidget* parent)
        : QDialog(parent), m_core(core), m_path(core->loadedConfigurationPath())
    {
        setWindowTitle(tr("Hardware Configuration"));
        resize(800, 520);
        if (!m_path.isEmpty()) { QString error; m_document = HardwareConfiguration::read(m_path, &error); if (!error.isEmpty()) QMessageBox::warning(this, windowTitle(), error); }
        m_original = m_document.toByteArray();
        auto* outer = new QVBoxLayout(this);
        auto* body = new QHBoxLayout;
        outer->addLayout(body);
        m_steps = new QListWidget(this);
        m_steps->addItems({tr("Devices"), tr("Roles"), tr("Light Sources"), tr("Apply")});
        m_steps->setMaximumWidth(150);
        body->addWidget(m_steps);
        m_pages = new QStackedWidget(this);
        body->addWidget(m_pages, 1);
        auto* devicesPage = new QWidget(this);
        auto* devicesLayout = new QVBoxLayout(devicesPage);
        m_devices = new QTreeWidget(devicesPage);
        m_devices->setHeaderLabels({tr("Label"), tr("Driver"), tr("Device")});
        m_devices->header()->setSectionResizeMode(QHeaderView::Stretch);
        devicesLayout->addWidget(m_devices);
        auto* controls = new QHBoxLayout;
        for (const auto& pair : {qMakePair(tr("Add"), 0), qMakePair(tr("Edit"), 1), qMakePair(tr("Remove"), 2)})
        {
            auto* button = new QPushButton(pair.first, devicesPage); controls->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, n = pair.second] { if (n == 0) addDevice(); else if (n == 1) editDevice(); else removeDevice(); });
        }
        controls->addStretch(); devicesLayout->addLayout(controls); m_pages->addWidget(devicesPage);
        auto* roles = new QWidget(this); auto* form = new QFormLayout(roles);
        m_camera = new QComboBox(roles); m_shutter = new QComboBox(roles); m_focus = new QComboBox(roles); m_autoShutter = new QComboBox(roles);
        for (auto* combo : {m_camera, m_shutter, m_focus}) combo->addItem(QString(), QString());
        for (const auto& d : core->hardwareDevices())
        {
            if (d.kind == HardwareDeviceKind::Camera) m_camera->addItem(d.name, d.logicalId);
            if (d.kind == HardwareDeviceKind::Shutter) m_shutter->addItem(d.name, d.logicalId);
            if (d.kind == HardwareDeviceKind::ZStage) m_focus->addItem(d.name, d.logicalId);
        }
        for (const QString& value : {QString(), QStringLiteral("0"), QStringLiteral("1")}) m_autoShutter->addItem(value, value);
        for (const auto& pair : {qMakePair(m_camera, QStringLiteral("Camera")), qMakePair(m_shutter, QStringLiteral("Shutter")), qMakePair(m_focus, QStringLiteral("Focus")), qMakePair(m_autoShutter, QStringLiteral("AutoShutter"))})
        {
            const QString value = m_document.coreRole(pair.second);
            if (pair.first->findData(value) < 0) pair.first->addItem(value, value);
            pair.first->setCurrentIndex(pair.first->findData(value));
            form->addRow(pair.second, pair.first);
        }
        m_pages->addWidget(roles);
        m_pages->addWidget(new LightSourcesPage(core, this));
        m_changes = new QPlainTextEdit(this); m_changes->setReadOnly(true); m_pages->addWidget(m_changes);
        connect(m_steps, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
        connect(m_steps, &QListWidget::currentRowChanged, this, [this](int row) { if (row == 3) updateChanges(); });
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
        connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &HardwareConfigurationDialog::apply);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        outer->addWidget(buttons); refreshDevices(); m_steps->setCurrentRow(0);
    }

    void HardwareConfigurationDialog::showLightSources() { m_steps->setCurrentRow(2); }
    void HardwareConfigurationDialog::refreshDevices()
    {
        m_devices->clear();
        for (const auto& label : m_document.microManagerDeviceLabels()) new QTreeWidgetItem(m_devices, {label, QStringLiteral("Micro-Manager"), {}});
        for (const auto& d : m_document.scopeOneDevices()) { auto* item = new QTreeWidgetItem(m_devices, {d.label, d.driverId, d.deviceName}); item->setData(0, Qt::UserRole, true); }
    }
    void HardwareConfigurationDialog::addDevice()
    {
        HardwareConfiguration::ScopeOneDevice d;
        if (!deviceDialog(this, d, true)) return;
        if (m_document.microManagerDeviceLabels().contains(d.label)) { QMessageBox::warning(this, windowTitle(), tr("Device label already exists.")); return; }
        for (const auto& old : m_document.scopeOneDevices()) if (old.label == d.label) { QMessageBox::warning(this, windowTitle(), tr("Device label already exists.")); return; }
        m_document.addScopeOneDevice(d); refreshDevices();
    }
    void HardwareConfigurationDialog::editDevice()
    {
        auto* item = m_devices->currentItem(); if (!item || !item->data(0, Qt::UserRole).toBool()) return;
        for (auto d : m_document.scopeOneDevices()) if (d.label == item->text(0))
        {
            const auto original = d.properties;
            if (!deviceDialog(this, d, false)) return;
            for (const auto& p : d.properties) if (!original.contains(p)) m_document.setScopeOneProperty(d.label, p.first, p.second);
            refreshDevices(); return;
        }
    }
    void HardwareConfigurationDialog::removeDevice()
    {
        auto* item = m_devices->currentItem(); if (!item || !item->data(0, Qt::UserRole).toBool()) return;
        m_document.removeScopeOneDevice(item->text(0)); refreshDevices();
    }
    bool HardwareConfigurationDialog::updateChanges()
    {
        for (const auto& pair : {qMakePair(m_camera, QStringLiteral("Camera")), qMakePair(m_shutter, QStringLiteral("Shutter")), qMakePair(m_focus, QStringLiteral("Focus")), qMakePair(m_autoShutter, QStringLiteral("AutoShutter"))})
            if (m_document.coreRole(pair.second) != pair.first->currentData().toString()) m_document.setCoreRole(pair.second, pair.first->currentData().toString());
        if (!m_path.isEmpty())
        {
            // Light source edits write immediately while this dialog holds device edits in memory.
            // Replay the staged device and role changes onto the current file before writing it.
            QString error;
            auto current = HardwareConfiguration::read(m_path, &error);
            if (!error.isEmpty())
            {
                QMessageBox::warning(this, windowTitle(), error);
                return false;
            }
            {
                const auto original = HardwareConfiguration::fromByteArray(m_original);
                const auto oldDevices = original.scopeOneDevices();
                const auto newDevices = m_document.scopeOneDevices();
                for (const auto& old : oldDevices)
                {
                    auto it = std::find_if(newDevices.cbegin(), newDevices.cend(), [&](const auto& d) { return d.label == old.label; });
                    if (it == newDevices.cend()) { current.removeScopeOneDevice(old.label); continue; }
                    for (const auto& p : old.properties) if (!it->properties.contains(p))
                    {
                        bool exists = false; for (const auto& n : it->properties) if (n.first == p.first) exists = true;
                        if (!exists) current.removeScopeOneProperty(old.label, p.first);
                    }
                    for (const auto& p : it->properties) if (!old.properties.contains(p)) current.setScopeOneProperty(old.label, p.first, p.second);
                }
                for (const auto& d : newDevices)
                    if (std::none_of(oldDevices.cbegin(), oldDevices.cend(), [&](const auto& old) { return old.label == d.label; })) current.addScopeOneDevice(d);
                for (const QString& role : {QStringLiteral("Camera"), QStringLiteral("Shutter"), QStringLiteral("Focus"), QStringLiteral("AutoShutter")})
                    if (m_document.coreRole(role) != original.coreRole(role)) current.setCoreRole(role, m_document.coreRole(role));
                m_document = std::move(current);
            }
        }
        const auto oldLines = m_original.split('\n'); const auto newLines = m_document.toByteArray().split('\n');
        QStringList lines;
        for (const auto& line : oldLines) if (!line.isEmpty() && !newLines.contains(line)) lines.append(QStringLiteral("- ") + QString::fromUtf8(line));
        for (const auto& line : newLines) if (!line.isEmpty() && !oldLines.contains(line)) lines.append(QStringLiteral("+ ") + QString::fromUtf8(line));
        m_changes->setPlainText(lines.join(QLatin1Char('\n')));
        return true;
    }
    void HardwareConfigurationDialog::apply()
    {
        if (!updateChanges()) return;
        m_steps->setCurrentRow(3);
        if (m_path.isEmpty()) m_path = QFileDialog::getSaveFileName(this, tr("Save Configuration"), {}, tr("Configuration (*.cfg)"));
        if (m_path.isEmpty()) return;
        if (QMessageBox::question(this, windowTitle(), tr("Write these changes and reload the configuration?")) != QMessageBox::Yes) return;
        QString error; if (!m_document.write(m_path, &error)) { QMessageBox::warning(this, windowTitle(), error); return; }
        accept();
    }
}

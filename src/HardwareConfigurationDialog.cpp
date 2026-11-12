#include "HardwareConfigurationDialog.h"
#include "scopeone/DriverManifest.h"
#include "scopeone/ScopeOneCore.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <functional>

namespace scopeone::ui
{
    using namespace scopeone::core;
    namespace
    {
        // Adds the device of a driver to a cfg file, or edits its properties. Core describes the
        // device names and pre-init properties of the driver.
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
            {
                if (d.error.isEmpty() && !runsDescriptions(d))
                {
                    driver->addItem(d.manifest.name, d.manifest.id);
                }
            }
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
            QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
            ok->setEnabled(false);
            auto* watcher = new QFutureWatcher<DriverDescription>(&dialog);
            QHash<QString, QComboBox*> editors;
            bool pendingDescription = false;

            // DriverHost --describe runs off the GUI thread. A request made while one runs is sent
            // once that one ends, so the result always matches the current selection.
            auto describe = [&]()
            {
                ok->setEnabled(false);
                if (watcher->isRunning())
                {
                    pendingDescription = true;
                    return;
                }
                pendingDescription = false;
                watcher->setFuture(QtConcurrent::run(describeDriver, driver->currentData().toString(), name->currentText()));
            };

            QObject::connect(watcher, &QFutureWatcher<DriverDescription>::finished, &dialog, [&]()
            {
                if (pendingDescription)
                {
                    describe();
                    return;
                }
                const DriverDescription result = watcher->result();
                if (!result.error.isEmpty())
                {
                    QMessageBox::warning(&dialog, QObject::tr("Driver"), result.error);
                    return;
                }
                const QString previousName = name->currentText();
                {
                    const QSignalBlocker blocker(name);
                    name->clear();
                    name->addItems(result.deviceNames);
                    if (!previousName.isEmpty())
                    {
                        name->setCurrentText(previousName);
                    }
                }
                // The first query lists devices; its empty name does not describe the selected device.
                if (previousName.isEmpty() && !name->currentText().isEmpty())
                {
                    describe();
                    return;
                }
                while (QLayoutItem* item = properties->takeAt(0))
                {
                    delete item->widget();
                    delete item;
                }
                editors.clear();
                for (const PreInitProperty& p : result.properties)
                {
                    auto* editor = new QComboBox(&dialog);
                    editor->setEditable(true);
                    editor->addItems(p.allowedValues);
                    QString initial = p.defaultValue;
                    for (const auto& old : device.properties)
                    {
                        if (old.first == p.name)
                        {
                            initial = old.second;
                        }
                    }
                    editor->setCurrentText(initial);
                    editors.insert(p.name, editor);
                    properties->addRow(p.name, editor);
                }
                ok->setEnabled(true);
            });
            QObject::connect(driver, &QComboBox::currentIndexChanged, &dialog, [&](int) { name->clear(); describe(); });
            QObject::connect(name, &QComboBox::activated, &dialog, [&](int) { describe(); });
            QObject::connect(name->lineEdit(), &QLineEdit::editingFinished, &dialog, [&]() { describe(); });
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]()
            {
                const QString labelText = label->text().trimmed();
                const QString nameText = name->currentText().trimmed();
                if (driver->currentIndex() < 0 || labelText.isEmpty() || nameText.isEmpty()
                    || !HardwareConfiguration::isValidField(labelText) || !HardwareConfiguration::isValidField(nameText))
                {
                    QMessageBox::warning(&dialog, QObject::tr("Device"),
                                         QObject::tr("Enter a driver, label and device name without commas or line breaks."));
                    return;
                }
                for (auto it = editors.cbegin(); it != editors.cend(); ++it)
                {
                    if (!HardwareConfiguration::isValidField(it.value()->currentText()))
                    {
                        QMessageBox::warning(&dialog, QObject::tr("Device"),
                                             QObject::tr("Property values cannot contain commas, line breaks or surrounding spaces."));
                        return;
                    }
                }
                dialog.accept();
            });
            // A device whose driver is missing had the first driver selected, and OK wrote that
            // driver's pre-init properties into the device
            if (!adding && driver->findData(device.driverId) < 0)
            {
                QMessageBox::warning(parent, QObject::tr("Edit Device"),
                                     QObject::tr("The driver %1 of %2 is not installed.").arg(device.driverId, device.label));
                return false;
            }
            {
                const QSignalBlocker blocker(driver);
                driver->setCurrentIndex(qMax(0, driver->findData(device.driverId)));
            }
            driver->setEnabled(adding);
            label->setReadOnly(!adding);
            name->setEnabled(adding);
            name->setCurrentText(device.deviceName);
            describe();
            if (dialog.exec() != QDialog::Accepted)
            {
                return false;
            }
            device.label = label->text().trimmed();
            device.driverId = driver->currentData().toString();
            device.deviceName = name->currentText().trimmed();
            for (auto it = editors.cbegin(); it != editors.cend(); ++it)
            {
                bool found = false;
                for (auto& property : device.properties)
                {
                    if (property.first == it.key())
                    {
                        property.second = it.value()->currentText();
                        found = true;
                    }
                }
                if (!found)
                {
                    device.properties.append({it.key(), it.value()->currentText()});
                }
            }
            return true;
        }

        // Picks the properties of a light source among those of the loaded devices: a writable number
        // for the power, a writable property with its on and off values for the switch, and a
        // read-only number for the measured power
        bool lightSourceDialog(QWidget* parent, ScopeOneCore* core, HardwareConfiguration::LightSource& source, bool existing)
        {
            QDialog dialog(parent);
            dialog.setWindowTitle(existing ? QObject::tr("Edit Light Source") : QObject::tr("Add Light Source"));
            auto* form = new QFormLayout(&dialog);
            auto* name = new QLineEdit(source.name, &dialog);
            name->setReadOnly(existing);
            form->addRow(QObject::tr("Name"), name);
            auto* power = new QComboBox(&dialog);
            auto* enable = new QComboBox(&dialog);
            auto* reading = new QComboBox(&dialog);
            enable->addItem(QObject::tr("None")); reading->addItem(QObject::tr("None"));
            for (const auto& device : core->loadedDevices())
            {
                for (const auto& property : core->deviceProperties(device, true))
                {
                    const QVariantList key{device, property.name()};
                    const QString label = QStringLiteral("%1 / %2 = %3").arg(device, property.name(), property.value());
                    const bool numeric = property.type().compare(QStringLiteral("Float"), Qt::CaseInsensitive) == 0
                        || property.type().compare(QStringLiteral("Integer"), Qt::CaseInsensitive) == 0;
                    if (numeric && !property.isReadOnly()) power->addItem(label, key);
                    if (!property.isReadOnly()) enable->addItem(label, key);
                    if (numeric && property.isReadOnly()) reading->addItem(label, key);
                }
            }
            auto select = [](QComboBox* combo, const QString& device, const QString& property) {
                if (property.isEmpty()) return;
                const QVariantList key{device, property};
                int index = combo->findData(key);
                // A configured property may be unavailable; preserve it when editing another field.
                if (index < 0) { combo->addItem(QStringLiteral("%1 / %2").arg(device, property), key); index = combo->count() - 1; }
                combo->setCurrentIndex(index);
            };
            select(power, source.powerDevice, source.powerProperty);
            select(enable, source.enableDevice, source.enableProperty);
            select(reading, source.readingDevice, source.readingProperty);
            form->addRow(QObject::tr("Power"), power);
            form->addRow(QObject::tr("Enable"), enable);
            auto* on = new QComboBox(&dialog);
            auto* off = new QComboBox(&dialog);
            form->addRow(QObject::tr("On value"), on); form->addRow(QObject::tr("Off value"), off);
            auto updateValues = [&] {
                on->clear(); off->clear();
                const auto key = enable->currentData().toList();
                if (key.size() != 2) return;
                for (const auto& property : core->deviceProperties(key[0].toString(), true))
                    if (property.name() == key[1].toString()) { on->addItems(property.allowedValues()); off->addItems(property.allowedValues()); break; }
                if (off->count() > 1) off->setCurrentIndex(1);
            };
            QObject::connect(enable, &QComboBox::currentIndexChanged, &dialog, [&](int) { updateValues(); });
            updateValues();
            if (!source.onValue.isEmpty() && on->findText(source.onValue) < 0) on->addItem(source.onValue);
            if (!source.offValue.isEmpty() && off->findText(source.offValue) < 0) off->addItem(source.offValue);
            if (existing) { on->setCurrentText(source.onValue); off->setCurrentText(source.offValue); }
            form->addRow(QObject::tr("Reading"), reading);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]()
            {
                const bool switched = enable->currentData().toList().size() == 2;
                if (name->text().trimmed().isEmpty() || !HardwareConfiguration::isValidField(name->text().trimmed())
                    || power->currentData().toList().size() != 2)
                {
                    QMessageBox::warning(&dialog, dialog.windowTitle(), QObject::tr("Enter a name without commas or line breaks and choose a power property."));
                    return;
                }
                if (switched && (on->currentText().isEmpty() || off->currentText().isEmpty() || on->currentText() == off->currentText()
                                 || !HardwareConfiguration::isValidField(on->currentText()) || !HardwareConfiguration::isValidField(off->currentText())))
                {
                    QMessageBox::warning(&dialog, dialog.windowTitle(), QObject::tr("Choose different on and off values."));
                    return;
                }
                dialog.accept();
            });
            form->addRow(buttons);
            if (dialog.exec() != QDialog::Accepted) return false;
            const auto p = power->currentData().toList();
            const auto e = enable->currentData().toList();
            const auto r = reading->currentData().toList();
            source.name = name->text().trimmed(); source.powerDevice = p[0].toString(); source.powerProperty = p[1].toString();
            source.enableDevice = e.size() == 2 ? e[0].toString() : QString{};
            source.enableProperty = e.size() == 2 ? e[1].toString() : QString{};
            source.onValue = e.size() == 2 ? on->currentText() : QString{};
            source.offValue = e.size() == 2 ? off->currentText() : QString{};
            source.readingDevice = r.size() == 2 ? r[0].toString() : QString{};
            source.readingProperty = r.size() == 2 ? r[1].toString() : QString{};
            return true;
        }
    }

    HardwareConfigurationDialog::HardwareConfigurationDialog(ScopeOneCore* core, QWidget* parent)
        : QDialog(parent), m_core(core), m_path(core->loadedConfigurationPath())
    {
        setWindowTitle(tr("Hardware Configuration"));
        resize(800, 520);
        if (!m_path.isEmpty())
        {
            QString error;
            m_document = HardwareConfiguration::read(m_path, &error);
            if (!error.isEmpty())
            {
                QMessageBox::warning(this, windowTitle(), error);
            }
        }
        m_original = m_document.toByteArray();

        auto* outer = new QVBoxLayout(this);
        auto* body = new QHBoxLayout;
        outer->addLayout(body);
        m_steps = new QListWidget(this);
        m_steps->addItems({tr("Devices"), tr("Roles")});
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
        const auto addButton = [&](const QString& text, void (HardwareConfigurationDialog::*slot)())
        {
            auto* button = new QPushButton(text, devicesPage);
            connect(button, &QPushButton::clicked, this, slot);
            controls->addWidget(button);
        };
        addButton(tr("Add"), &HardwareConfigurationDialog::addDevice);
        addButton(tr("Edit"), &HardwareConfigurationDialog::editDevice);
        addButton(tr("Remove"), &HardwareConfigurationDialog::removeDevice);
        controls->addStretch();
        devicesLayout->addLayout(controls);
        m_pages->addWidget(devicesPage);

        auto* roles = new QWidget(this);
        auto* rolesLayout = new QVBoxLayout(roles);
        auto* form = new QFormLayout;
        rolesLayout->addLayout(form);
        const auto devices = core->hardwareDevices();
        const QList<std::pair<QString, HardwareDeviceKind>> deviceRoles{
            {QStringLiteral("Camera"), HardwareDeviceKind::Camera},
            {QStringLiteral("Shutter"), HardwareDeviceKind::Shutter},
            {QStringLiteral("Focus"), HardwareDeviceKind::ZStage},
            {QStringLiteral("XYStage"), HardwareDeviceKind::XYStage},
            {QStringLiteral("AutoFocus"), HardwareDeviceKind::AutoFocus}};
        for (const auto& [role, kind] : deviceRoles)
        {
            auto* combo = new QComboBox(roles);
            combo->addItem(QString(), QString());
            for (const auto& d : devices)
            {
                if (d.kind == kind) combo->addItem(d.name, d.logicalId);
            }
            m_roles.append({role, combo});
        }
        auto* autoShutter = new QComboBox(roles);
        for (const QString& value : {QString(), QStringLiteral("0"), QStringLiteral("1")})
        {
            autoShutter->addItem(value, value);
        }
        m_roles.append({QStringLiteral("AutoShutter"), autoShutter});
        for (const auto& [role, combo] : m_roles)
        {
            const QString value = m_document.coreRole(role);
            if (combo->findData(value) < 0)
            {
                combo->addItem(value, value);
            }
            combo->setCurrentIndex(combo->findData(value));
            form->addRow(role, combo);
        }
        rolesLayout->addWidget(new QLabel(tr("Light Sources"), roles));
        m_lightSources = new QListWidget(roles);
        rolesLayout->addWidget(m_lightSources, 1);
        auto* lightControls = new QHBoxLayout;
        const auto addLightButton = [&](const QString& text, const std::function<void()>& action)
        {
            auto* button = new QPushButton(text, roles);
            connect(button, &QPushButton::clicked, this, action);
            lightControls->addWidget(button);
        };
        addLightButton(tr("Add"), [this] { editLightSource(false); });
        addLightButton(tr("Edit"), [this] { editLightSource(true); });
        addLightButton(tr("Remove"), [this]
        {
            if (m_lightSources->currentItem())
            {
                m_document.removeLightSource(m_lightSources->currentItem()->text());
                refreshLightSources();
            }
        });
        lightControls->addStretch();
        rolesLayout->addLayout(lightControls);
        connect(m_lightSources, &QListWidget::itemDoubleClicked, this, [this] { editLightSource(true); });
        refreshLightSources();
        m_pages->addWidget(roles);
        connect(m_steps, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
        connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &HardwareConfigurationDialog::apply);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        outer->addWidget(buttons);
        refreshDevices();
        m_steps->setCurrentRow(0);
    }

    void HardwareConfigurationDialog::showRoles()
    {
        m_steps->setCurrentRow(1);
    }

    void HardwareConfigurationDialog::refreshLightSources()
    {
        m_lightSources->clear();
        for (const auto& source : m_document.lightSources())
        {
            m_lightSources->addItem(source.name);
        }
    }

    void HardwareConfigurationDialog::editLightSource(bool existing)
    {
        HardwareConfiguration::LightSource source;
        const auto sources = m_document.lightSources();
        if (existing)
        {
            if (!m_lightSources->currentItem()) return;
            for (const auto& item : sources)
                if (item.name == m_lightSources->currentItem()->text()) source = item;
        }
        if (!lightSourceDialog(this, m_core, source, existing)) return;
        if (!existing && std::any_of(sources.cbegin(), sources.cend(), [&](const auto& item) { return item.name == source.name; }))
        {
            QMessageBox::warning(this, windowTitle(), tr("A light source with this name already exists."));
            return;
        }
        m_document.setLightSource(source);
        refreshLightSources();
    }

    void HardwareConfigurationDialog::refreshDevices()
    {
        m_devices->clear();
        for (const auto& label : m_document.microManagerDeviceLabels())
        {
            new QTreeWidgetItem(m_devices, {label, QStringLiteral("Micro-Manager"), {}});
        }
        for (const auto& d : m_document.scopeOneDevices())
        {
            auto* item = new QTreeWidgetItem(m_devices, {d.label, d.driverId, d.deviceName});
            item->setData(0, Qt::UserRole, true);
        }
    }

    void HardwareConfigurationDialog::addDevice()
    {
        HardwareConfiguration::ScopeOneDevice d;
        if (!deviceDialog(this, d, true))
        {
            return;
        }
        const auto devices = m_document.scopeOneDevices();
        const bool exists = m_document.microManagerDeviceLabels().contains(d.label)
            || std::any_of(devices.cbegin(), devices.cend(), [&](const auto& old) { return old.label == d.label; });
        if (exists)
        {
            QMessageBox::warning(this, windowTitle(), tr("Device label already exists."));
            return;
        }
        m_document.addScopeOneDevice(d);
        refreshDevices();
    }

    void HardwareConfigurationDialog::editDevice()
    {
        auto* item = m_devices->currentItem();
        if (!item || !item->data(0, Qt::UserRole).toBool())
        {
            return;
        }
        for (auto d : m_document.scopeOneDevices())
        {
            if (d.label != item->text(0))
            {
                continue;
            }
            const auto original = d.properties;
            if (!deviceDialog(this, d, false))
            {
                return;
            }
            for (const auto& p : d.properties)
            {
                if (!original.contains(p))
                {
                    m_document.setScopeOneProperty(d.label, p.first, p.second);
                }
            }
            refreshDevices();
            return;
        }
    }

    void HardwareConfigurationDialog::removeDevice()
    {
        auto* item = m_devices->currentItem();
        if (!item || !item->data(0, Qt::UserRole).toBool())
        {
            return;
        }
        m_document.removeScopeOneDevice(item->text(0));
        refreshDevices();
    }

    void HardwareConfigurationDialog::readRoles()
    {
        for (const auto& [role, combo] : m_roles)
        {
            const QString value = combo->currentData().toString();
            if (m_document.coreRole(role) != value)
            {
                m_document.setCoreRole(role, value);
            }
        }
    }

    // The lines that differ between the file as opened and the document, as a diff
    QString HardwareConfigurationDialog::changes() const
    {
        const auto oldLines = m_original.split('\n');
        const auto newLines = m_document.toByteArray().split('\n');
        QStringList lines;
        for (const auto& line : oldLines)
        {
            if (!line.isEmpty() && !newLines.contains(line)) lines.append(QStringLiteral("- ") + QString::fromUtf8(line));
        }
        for (const auto& line : newLines)
        {
            if (!line.isEmpty() && !oldLines.contains(line)) lines.append(QStringLiteral("+ ") + QString::fromUtf8(line));
        }
        return lines.join(QLatin1Char('\n'));
    }

    void HardwareConfigurationDialog::apply()
    {
        readRoles();
        // Only a change of the devices restarts them. The roles and light sources are taken again
        // from the written file.
        m_reload = m_path.isEmpty()
            || HardwareConfiguration::fromByteArray(m_original).scopeOneDevices() != m_document.scopeOneDevices();
        if (m_path.isEmpty())
        {
            m_path = QFileDialog::getSaveFileName(this, tr("Save Configuration"), {}, tr("Configuration (*.cfg)"));
        }
        if (m_path.isEmpty())
        {
            return;
        }
        // The diff of a new or large cfg is longer than the screen, so it goes in the scrolling details
        QMessageBox question(QMessageBox::Question, windowTitle(),
                             m_reload ? tr("Write these changes and reload the configuration?") : tr("Write these changes?"),
                             QMessageBox::Yes | QMessageBox::No, this);
        question.setDetailedText(changes());
        if (question.exec() != QMessageBox::Yes)
        {
            return;
        }
        QString error;
        if (!m_document.write(m_path, &error))
        {
            QMessageBox::warning(this, windowTitle(), error);
            return;
        }
        accept();
    }
}

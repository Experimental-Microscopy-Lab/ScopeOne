#include "LightSourcesPage.h"
#include "scopeone/ScopeOneCore.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace scopeone::ui
{
    using Core = scopeone::core::ScopeOneCore;

    LightSourcesPage::LightSourcesPage(Core* core, QWidget* parent) : QWidget(parent), m_core(core)
    {
        auto* layout = new QVBoxLayout(this);
        m_list = new QListWidget(this);
        layout->addWidget(m_list);
        auto* buttons = new QHBoxLayout;
        auto* add = new QPushButton(tr("Add"), this);
        auto* change = new QPushButton(tr("Edit"), this);
        auto* remove = new QPushButton(tr("Remove"), this);
        buttons->addWidget(add); buttons->addWidget(change); buttons->addWidget(remove); buttons->addStretch();
        layout->addLayout(buttons);
        connect(add, &QPushButton::clicked, this, [this] { edit(false); });
        connect(change, &QPushButton::clicked, this, [this] { edit(true); });
        connect(m_list, &QListWidget::itemDoubleClicked, this, [this] { edit(true); });
        connect(remove, &QPushButton::clicked, this, [this] {
            if (!m_list->currentItem()) return;
            QString error;
            if (!m_core->removeLightSource(m_list->currentItem()->text(), &error))
                QMessageBox::warning(this, tr("Light Sources"), error);
        });
        connect(m_core, &Core::hardwareConfigurationChanged, this, [this] { refresh(); });
        refresh();
    }

    void LightSourcesPage::refresh()
    {
        const QString selected = m_list->currentItem() ? m_list->currentItem()->text() : QString{};
        m_list->clear();
        for (const auto& source : m_core->lightSources()) m_list->addItem(source.name);
        const auto matches = m_list->findItems(selected, Qt::MatchExactly);
        if (!matches.isEmpty()) m_list->setCurrentItem(matches.first());
    }

    void LightSourcesPage::edit(bool existing)
    {
        Core::LightSource source;
        if (existing)
        {
            if (!m_list->currentItem()) return;
            for (const auto& item : m_core->lightSources())
                if (item.name == m_list->currentItem()->text()) { source = item; break; }
        }
        QDialog dialog(this);
        dialog.setWindowTitle(existing ? tr("Edit Light Source") : tr("Add Light Source"));
        auto* form = new QFormLayout(&dialog);
        auto* name = new QLineEdit(source.name, &dialog);
        if (existing) name->setReadOnly(true);
        form->addRow(tr("Name"), name);
        auto* power = new QComboBox(&dialog);
        auto* enable = new QComboBox(&dialog);
        auto* reading = new QComboBox(&dialog);
        enable->addItem(tr("None")); reading->addItem(tr("None"));
        for (const auto& device : m_core->loadedDevices())
        {
            for (const auto& property : m_core->deviceProperties(device, false))
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
        form->addRow(tr("Power"), power);
        form->addRow(tr("Enable"), enable);
        auto* on = new QComboBox(&dialog);
        auto* off = new QComboBox(&dialog);
        form->addRow(tr("On value"), on); form->addRow(tr("Off value"), off);
        auto updateValues = [&] {
            on->clear(); off->clear();
            const auto key = enable->currentData().toList();
            if (key.size() != 2) return;
            for (const auto& property : m_core->deviceProperties(key[0].toString(), false))
                if (property.name() == key[1].toString()) { on->addItems(property.allowedValues()); off->addItems(property.allowedValues()); break; }
            if (off->count() > 1) off->setCurrentIndex(1);
        };
        connect(enable, &QComboBox::currentIndexChanged, &dialog, [&](int) { updateValues(); });
        updateValues();
        if (!source.onValue.isEmpty() && on->findText(source.onValue) < 0) on->addItem(source.onValue);
        if (!source.offValue.isEmpty() && off->findText(source.offValue) < 0) off->addItem(source.offValue);
        if (existing) { on->setCurrentText(source.onValue); off->setCurrentText(source.offValue); }
        form->addRow(tr("Reading"), reading);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        form->addRow(buttons);
        if (dialog.exec() != QDialog::Accepted) return;
        const auto p = power->currentData().toList();
        const auto e = enable->currentData().toList();
        const auto r = reading->currentData().toList();
        if (name->text().trimmed().isEmpty() || p.size() != 2) return;
        if (e.size() == 2 && (on->currentText().isEmpty() || off->currentText().isEmpty()
                              || on->currentText() == off->currentText()))
        {
            QMessageBox::warning(this, tr("Light Sources"), tr("Choose different on and off values."));
            return;
        }
        if (!existing)
            for (const auto& item : m_core->lightSources())
                if (item.name == name->text().trimmed())
                { QMessageBox::warning(this, tr("Light Sources"), tr("A light source with this name already exists.")); return; }
        source.name = name->text().trimmed(); source.powerDevice = p[0].toString(); source.powerProperty = p[1].toString();
        source.enableDevice = e.size() == 2 ? e[0].toString() : QString{};
        source.enableProperty = e.size() == 2 ? e[1].toString() : QString{};
        source.onValue = on->currentText(); source.offValue = off->currentText();
        source.readingDevice = r.size() == 2 ? r[0].toString() : QString{};
        source.readingProperty = r.size() == 2 ? r[1].toString() : QString{};
        QString error;
        if (!m_core->setLightSource(source, &error)) QMessageBox::warning(this, tr("Light Sources"), error);
    }
}

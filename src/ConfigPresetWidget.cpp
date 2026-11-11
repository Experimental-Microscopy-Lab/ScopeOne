#include "ConfigPresetWidget.h"

#include "scopeone/ScopeOneCore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QAction>
#include <QEvent>
#include <QHash>
#include <QLineEdit>
#include <QScopedValueRollback>
#include <QSet>
#include <QToolBar>
#include <QHeaderView>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <functional>

namespace scopeone::ui
{
    namespace
    {
        class NoWheelComboBox : public QComboBox
        {
        public:
            using QComboBox::QComboBox;

        protected:
            // Ignore wheel edits inside preset combo boxes
            void wheelEvent(QWheelEvent* event) override
            {
                event->ignore();
            }
        };

        class PresetComboBox : public NoWheelComboBox
        {
        public:
            using NoWheelComboBox::NoWheelComboBox;
            std::function<void()> selectRow;
        protected:
            void focusInEvent(QFocusEvent* event) override
            {
                if (selectRow) selectRow();
                NoWheelComboBox::focusInEvent(event);
            }
            void mousePressEvent(QMouseEvent* event) override
            {
                if (selectRow) selectRow();
                NoWheelComboBox::mousePressEvent(event);
            }
        };
    }

    namespace
    {
        using Setting = scopeone::core::ConfigSetting;
        using Core = scopeone::core::ScopeOneCore;
        QString settingKey(const QString& device, const QString& property)
        { return device + QChar(0x1f) + property; }

        QList<Setting> groupSettings(Core* core, const QString& group)
        {
            QList<Setting> result;
            QSet<QString> seen;
            for (const auto& preset : core->availableConfigs(group))
                for (const auto& setting : core->configPresetSettings(group, preset))
                    if (!seen.contains(settingKey(setting.device, setting.property)))
                    { seen.insert(settingKey(setting.device, setting.property)); result.append(setting); }
            return result;
        }

        bool editSettings(QWidget* parent, Core* core, const QString& title,
                          const QList<Setting>& initial, bool groupMode, QList<Setting>& result)
        {
            QDialog dialog(parent);
            dialog.setWindowTitle(title);
            dialog.resize(760, 560);
            auto* layout = new QVBoxLayout(&dialog);
            auto* tree = new QTreeWidget(&dialog);
            tree->setHeaderLabels({QObject::tr("Device / Property"), QObject::tr("Value"), QObject::tr("Details")});
            tree->setAlternatingRowColors(true);
            tree->header()->resizeSection(0, 300);
            tree->header()->resizeSection(1, 190);
            layout->addWidget(tree);
            QHash<QString, Setting> selected;
            for (const auto& setting : initial) selected.insert(settingKey(setting.device, setting.property), setting);
            QHash<QString, QLineEdit*> textEditors;
            QHash<QString, QComboBox*> choiceEditors;
            QSet<QString> found;
            for (const auto& device : core->loadedDevices())
            {
                QTreeWidgetItem* deviceItem = nullptr;
                for (const auto& property : core->deviceProperties(device, true))
                {
                    const QString key = settingKey(device, property.name());
                    if (!groupMode && !selected.contains(key)) continue;
                    if (!deviceItem) deviceItem = new QTreeWidgetItem(tree, {device});
                    auto* item = new QTreeWidgetItem(deviceItem, {property.name(), {}, {}});
                    item->setData(0, Qt::UserRole, key);
                    item->setData(0, Qt::UserRole + 1, device);
                    item->setData(0, Qt::UserRole + 2, property.name());
                    if (groupMode)
                    {
                        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                        item->setCheckState(0, selected.contains(key) ? Qt::Checked : Qt::Unchecked);
                        if ((property.isReadOnly() || property.isPreInit()) && !selected.contains(key)) item->setDisabled(true);
                    }
                    found.insert(key);
                    const QString value = groupMode ? property.value()
                        : selected.contains(key) ? selected.value(key).value : property.value();
                    if (!groupMode && !property.allowedValues().isEmpty())
                    {
                        auto* editor = new NoWheelComboBox(tree);
                        editor->addItems(property.allowedValues());
                        if (editor->findText(value) < 0) editor->addItem(value);
                        editor->setCurrentText(value);
                        tree->setItemWidget(item, 1, editor);
                        choiceEditors.insert(key, editor);
                    }
                    else
                    {
                        auto* editor = new QLineEdit(value, tree);
                        editor->setReadOnly(groupMode);
                        tree->setItemWidget(item, 1, editor);
                        textEditors.insert(key, editor);
                    }
                    QString details = property.type();
                    if (property.isReadOnly()) details += QObject::tr(" · Read only");
                    if (property.isPreInit()) details += QObject::tr(" · Pre-init");
                    if (property.hasLimits()) details += QStringLiteral(" [%1, %2]").arg(property.lowerLimit()).arg(property.upperLimit());
                    item->setText(2, details);
                }
            }
            for (auto it = selected.cbegin(); it != selected.cend(); ++it)
            {
                if (found.contains(it.key())) continue;
                auto* device = new QTreeWidgetItem(tree, {it->device});
                auto* item = new QTreeWidgetItem(device, {it->property, it->value, QObject::tr("Unavailable property")});
                item->setData(0, Qt::UserRole, it.key());
                item->setData(0, Qt::UserRole + 1, it->device);
                item->setData(0, Qt::UserRole + 2, it->property);
                if (groupMode) { item->setFlags(item->flags() | Qt::ItemIsUserCheckable); item->setCheckState(0, Qt::Checked); }
            }
            tree->expandAll();
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            layout->addWidget(buttons);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]()
            {
                result.clear();
                for (int i = 0; i < tree->topLevelItemCount(); ++i)
                    for (int j = 0; j < tree->topLevelItem(i)->childCount(); ++j)
                    {
                        auto* item = tree->topLevelItem(i)->child(j);
                        if (groupMode && item->checkState(0) != Qt::Checked) continue;
                        const QString key = item->data(0, Qt::UserRole).toString();
                        const QString value = choiceEditors.contains(key) ? choiceEditors.value(key)->currentText()
                            : textEditors.contains(key) ? textEditors.value(key)->text() : item->text(1);
                        result.append({item->data(0, Qt::UserRole + 1).toString(), item->data(0, Qt::UserRole + 2).toString(), value});
                    }
                if (result.isEmpty()) { QMessageBox::warning(&dialog, title, QObject::tr("Select at least one property.")); return; }
                dialog.accept();
            });
            return dialog.exec() == QDialog::Accepted;
        }
    }

    // Create the preset browser around the shared core facade
    ConfigPresetWidget::ConfigPresetWidget(scopeone::core::ScopeOneCore* core, QWidget* parent)
        : QWidget(parent)
          , m_scopeonecore(core)
    {
        if (!core)
        {
            qFatal("ConfigPresetWidget requires ScopeOneCore");
        }

        setupUI();

        m_autoRefreshTimer = new QTimer(this);
        m_autoRefreshTimer->setInterval(1000);
        connect(m_autoRefreshTimer, &QTimer::timeout, this, &ConfigPresetWidget::onAutoRefreshTimer);
    }

    // Build the preset table and refresh controls
    void ConfigPresetWidget::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);

        auto* controlLayout = new QHBoxLayout();
        auto* refreshButton = new QPushButton("Refresh", this);
        refreshButton->setMaximumWidth(60);
        connect(refreshButton, &QPushButton::clicked, this, &ConfigPresetWidget::refresh);

        auto* autoRefreshCheckBox = new QCheckBox("Auto Refresh", this);
        autoRefreshCheckBox->setChecked(m_autoRefresh);
        connect(autoRefreshCheckBox, &QCheckBox::toggled, this, &ConfigPresetWidget::onAutoRefreshToggled);

        controlLayout->addWidget(refreshButton);
        controlLayout->addWidget(autoRefreshCheckBox);
        auto* editToolbar = new QToolBar(this);
        auto addButton = [this, editToolbar](const QString& text, auto slot)
        {
            auto* action = editToolbar->addAction(text);
            connect(action, &QAction::triggered, this, slot);
        };
        addButton(tr("New Group"), &ConfigPresetWidget::addGroup);
        addButton(tr("Edit Group"), &ConfigPresetWidget::editGroup);
        addButton(tr("Delete Group"), &ConfigPresetWidget::removeGroup);
        addButton(tr("Rename Group"), &ConfigPresetWidget::renameGroup);
        addButton(tr("New Preset"), &ConfigPresetWidget::addPreset);
        addButton(tr("Edit Preset"), &ConfigPresetWidget::editPreset);
        addButton(tr("Delete Preset"), &ConfigPresetWidget::removePreset);
        addButton(tr("Rename Preset"), &ConfigPresetWidget::renamePreset);
        controlLayout->addStretch();

        m_configTree = new QTreeWidget(this);
        m_configTree->setHeaderLabels({"Group", "Preset"});
        m_configTree->setAlternatingRowColors(true);
        m_configTree->setRootIsDecorated(false);
        m_configTree->setSortingEnabled(true);
        m_configTree->sortByColumn(0, Qt::AscendingOrder);
        m_configTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_configTree->header()->resizeSection(0, 220);
        m_configTree->header()->resizeSection(1, 220);

        mainLayout->addLayout(controlLayout);
        mainLayout->addWidget(editToolbar);
        mainLayout->addWidget(m_configTree);
    }

    // Rebuild the preset table while preserving scroll position
    void ConfigPresetWidget::refresh()
    {
        if (m_updating || m_editing || m_scopeonecore->configurationOperationRunning())
        {
            return;
        }

        const int oldScrollValue = m_configTree->verticalScrollBar()->value();
        const QString oldGroup = selectedGroup();

        m_updating = true;
        m_configTree->clear();

        try
        {
            populateConfigTree();
            for (int i = 0; i < m_configTree->topLevelItemCount(); ++i)
                if (m_configTree->topLevelItem(i)->text(0) == oldGroup)
                    m_configTree->setCurrentItem(m_configTree->topLevelItem(i));
        }
        catch (const std::exception& e)
        {
            emit errorOccurred(QString("Error refreshing config presets: %1").arg(e.what()));
        }

        QTimer::singleShot(0, this, [this, oldScrollValue]()
        {
            m_configTree->verticalScrollBar()->setValue(oldScrollValue);
        });

        m_updating = false;
    }

    // Add every config group and editable preset selector
    void ConfigPresetWidget::populateConfigTree()
    {
        const QStringList groups = m_scopeonecore->availableConfigGroups();
        for (const QString& group : groups)
        {
            QTreeWidgetItem* item = new QTreeWidgetItem(m_configTree);
            item->setText(0, group);

            const QStringList presets = m_scopeonecore->availableConfigs(group);
            const QString currentPreset = m_scopeonecore->currentConfig(group);

            auto* combo = new PresetComboBox(m_configTree);
            combo->selectRow = [this, item] { m_configTree->setCurrentItem(item); };
            for (const QString& preset : presets)
            {
                combo->addItem(preset, true);
            }
            combo->setEnabled(!presets.isEmpty());

            const int currentIndex = combo->findText(currentPreset);
            if (currentIndex >= 0)
            {
                combo->setCurrentIndex(currentIndex);
            }
            else
            {
                combo->insertItem(0, tr("Custom"), false);
                combo->setCurrentIndex(0);
            }

            connect(combo, &QComboBox::currentTextChanged, this,
                    [this, group, combo](const QString& preset)
                    {
                        for (int i = 0; i < m_configTree->topLevelItemCount(); ++i)
                            if (m_configTree->topLevelItem(i)->text(0) == group)
                                m_configTree->setCurrentItem(m_configTree->topLevelItem(i));
                        if (m_updating || !combo->currentData().toBool())
                        {
                            return;
                        }
                        QString errorMessage;
                        if (!m_scopeonecore->setConfig(group, preset, &errorMessage))
                        {
                            emit errorOccurred(errorMessage);
                            refresh();
                            return;
                        }
                        emit configChanged(group, preset);
                    });

            m_configTree->setItemWidget(item, 1, combo);
        }
    }


    // Toggle periodic config preset refresh
    void ConfigPresetWidget::onAutoRefreshToggled(bool enabled)
    {
        m_autoRefresh = enabled;
        if (enabled)
        {
            m_autoRefreshTimer->start();
            return;
        }
        m_autoRefreshTimer->stop();
    }

    // Refresh presets when the timer fires
    void ConfigPresetWidget::onAutoRefreshTimer()
    {
        if (!m_updating)
        {
            refresh();
        }
    }

    QString ConfigPresetWidget::selectedGroup() const
    { const auto* item = m_configTree->currentItem(); return item ? item->text(0) : QString{}; }
    QString ConfigPresetWidget::selectedPreset() const
    {
        auto* item = m_configTree->currentItem();
        if (!item) return {};
        auto* combo = qobject_cast<QComboBox*>(m_configTree->itemWidget(item, 1));
        return combo && combo->currentData().toBool() ? combo->currentText() : QString{};
    }

    QString ConfigPresetWidget::choosePreset(const QString& group, const QString& title)
    {
        const QStringList presets = m_scopeonecore->availableConfigs(group);
        if (presets.isEmpty()) return {};
        bool accepted = false;
        const int current = qMax(0, presets.indexOf(selectedPreset()));
        const QString preset = QInputDialog::getItem(this, title, tr("Preset:"), presets, current, false, &accepted);
        return accepted ? preset : QString{};
    }

    void ConfigPresetWidget::finishEdit(const QString& group, const QString& preset)
    {
        // Permit a refresh while a modal editor's guard is still alive.
        QScopedValueRollback<bool> guard(m_editing, false);
        refresh();
        for (int i = 0; i < m_configTree->topLevelItemCount(); ++i)
            if (m_configTree->topLevelItem(i)->text(0) == group)
                m_configTree->setCurrentItem(m_configTree->topLevelItem(i));
        emit configChanged(group, preset);
    }

    void ConfigPresetWidget::addGroup()
    {
        QScopedValueRollback<bool> guard(m_editing, true);
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New Group"), tr("Group name:"), QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        if (m_scopeonecore->availableConfigGroups().contains(name))
        { emit errorOccurred(tr("Group already exists: %1").arg(name)); return; }
        QList<Setting> properties;
        if (!editSettings(this, m_scopeonecore, tr("Properties of %1").arg(name), {}, true, properties)) return;
        QString error;
        if (!m_scopeonecore->addConfigGroup(name, &error)) { emit errorOccurred(error); return; }
        if (!m_scopeonecore->setConfigGroupProperties(name, properties, &error))
        {
            QString cleanupError;
            if (!m_scopeonecore->removeConfigGroup(name, &cleanupError)) error += tr("; cleanup: %1").arg(cleanupError);
            emit errorOccurred(error);
            finishEdit(name);
            return;
        }
        finishEdit(name);
    }

    void ConfigPresetWidget::editGroup()
    {
        QScopedValueRollback<bool> guard(m_editing, true);
        const QString group = selectedGroup();
        if (group.isEmpty()) return;
        QList<Setting> properties;
        if (!editSettings(this, m_scopeonecore, tr("Properties of %1").arg(group),
                          groupSettings(m_scopeonecore, group), true, properties)) return;
        QString error;
        if (!m_scopeonecore->setConfigGroupProperties(group, properties, &error)) emit errorOccurred(error);
        else finishEdit(group);
    }
    void ConfigPresetWidget::removeGroup()
    {
        QScopedValueRollback<bool> guard(m_editing, true);
        const QString g=selectedGroup(); if(g.isEmpty()) return;
        if (QMessageBox::question(this, tr("Delete Group"),
                                  tr("Delete group %1 and all its presets?").arg(g)) != QMessageBox::Yes) return;
        QString error; if(!m_scopeonecore->removeConfigGroup(g,&error)) emit errorOccurred(error); else finishEdit(g);
    }
    void ConfigPresetWidget::renameGroup()
    {
        QScopedValueRollback<bool> guard(m_editing, true);
        const QString g=selectedGroup(); if(g.isEmpty()) return; bool ok=false; const QString n=QInputDialog::getText(this,tr("Rename Group"),tr("New name:"),QLineEdit::Normal,g,&ok).trimmed(); if(!ok||n.isEmpty()||n==g) return;
        QString error; if(!m_scopeonecore->renameConfigGroup(g,n,&error)) emit errorOccurred(error); else finishEdit(n);
    }
    void ConfigPresetWidget::addPreset()
    {
        QScopedValueRollback<bool> guard(m_editing, true);
        const QString g=selectedGroup(); if(g.isEmpty()) return; bool ok=false; const QString p=QInputDialog::getText(this,tr("New Preset"),tr("Preset name:"),QLineEdit::Normal,{},&ok).trimmed(); if(!ok||p.isEmpty()) return;
        if (m_scopeonecore->availableConfigs(g).contains(p)) { emit errorOccurred(tr("Preset already exists: %1").arg(p)); return; }
        QList<Setting> initial = groupSettings(m_scopeonecore, g);
        for (auto& setting : initial)
            setting.value = m_scopeonecore->getPropertyValue(setting.device, setting.property, true);
        QList<Setting> settings;
        // An empty group chooses its properties here; otherwise only its own properties appear.
        if (initial.isEmpty())
        {
            if (!editSettings(this, m_scopeonecore, tr("Properties of %1").arg(g), {}, true, initial)) return;
        }
        if (!editSettings(this, m_scopeonecore, tr("New preset %1").arg(p), initial, false, settings)) return;
        QString error; if(!m_scopeonecore->setConfigPreset(g,p,settings,&error)) emit errorOccurred(error); else finishEdit(g,p);
    }

    void ConfigPresetWidget::editPreset()
    {
        QScopedValueRollback<bool> guard(m_editing, true);
        const QString group = selectedGroup();
        if (group.isEmpty()) return;
        const QString preset = choosePreset(group, tr("Edit Preset"));
        if (preset.isEmpty()) return;
        QList<Setting> settings;
        if (!editSettings(this, m_scopeonecore, tr("Edit %1 / %2").arg(group, preset),
                          m_scopeonecore->configPresetSettings(group, preset), false, settings)) return;
        QString error;
        if (!m_scopeonecore->setConfigPreset(group, preset, settings, &error)) emit errorOccurred(error);
        else finishEdit(group, preset);
    }
    void ConfigPresetWidget::removePreset()
    {
        QScopedValueRollback<bool> guard(m_editing, true);
        const QString g=selectedGroup(); if(g.isEmpty()) return;
        const QString p=choosePreset(g,tr("Delete Preset")); if(p.isEmpty()) return;
        if (QMessageBox::question(this, tr("Delete Preset"), tr("Delete preset %1 / %2?").arg(g,p)) != QMessageBox::Yes) return;
        QString error; if(!m_scopeonecore->removeConfigPreset(g,p,&error)) emit errorOccurred(error); else finishEdit(g);
    }
    void ConfigPresetWidget::renamePreset()
    {
        QScopedValueRollback<bool> guard(m_editing, true);
        const QString g=selectedGroup(); if(g.isEmpty()) return;
        const QString p=choosePreset(g,tr("Rename Preset")); if(p.isEmpty()) return;
        bool ok=false; const QString n=QInputDialog::getText(this,tr("Rename Preset"),tr("New name:"),QLineEdit::Normal,p,&ok).trimmed(); if(!ok||n.isEmpty()||n==p) return;
        QString error; if(!m_scopeonecore->renameConfigPreset(g,p,n,&error)) emit errorOccurred(error); else finishEdit(g,n);
    }
}

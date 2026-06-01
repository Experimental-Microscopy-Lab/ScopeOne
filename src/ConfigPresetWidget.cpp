#include "ConfigPresetWidget.h"

#include "scopeone/ScopeOneCore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace scopeone::ui
{
    namespace
    {
        class NoWheelComboBox : public QComboBox
        {
        public:
            using QComboBox::QComboBox;

        protected:
            void wheelEvent(QWheelEvent* event) override
            {
                event->ignore();
            }
        };
    }

    ConfigPresetWidget::ConfigPresetWidget(scopeone::core::ScopeOneCore* core, QWidget* parent)
        : QWidget(parent)
          , m_scopeonecore(core)
    {
        if (!m_scopeonecore)
        {
            qFatal("ConfigPresetWidget requires ScopeOneCore");
        }

        setupUI();

        m_autoRefreshTimer = new QTimer(this);
        m_autoRefreshTimer->setInterval(1000);
        connect(m_autoRefreshTimer, &QTimer::timeout, this, &ConfigPresetWidget::onAutoRefreshTimer);
    }

    void ConfigPresetWidget::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);

        auto* controlLayout = new QHBoxLayout();
        auto* refreshButton = new QPushButton("Refresh", this);
        refreshButton->setMaximumWidth(60);
        connect(refreshButton, &QPushButton::clicked, this, &ConfigPresetWidget::onRefreshClicked);

        auto* autoRefreshCheckBox = new QCheckBox("Auto Refresh", this);
        autoRefreshCheckBox->setChecked(m_autoRefresh);
        connect(autoRefreshCheckBox, &QCheckBox::toggled, this, &ConfigPresetWidget::onAutoRefreshToggled);

        controlLayout->addWidget(refreshButton);
        controlLayout->addWidget(autoRefreshCheckBox);
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
        mainLayout->addWidget(m_configTree);
    }

    void ConfigPresetWidget::refresh()
    {
        if (m_updating || !m_configTree)
        {
            return;
        }

        const int oldScrollValue =
            m_configTree->verticalScrollBar() ? m_configTree->verticalScrollBar()->value() : 0;

        m_updating = true;
        m_configTree->clear();

        try
        {
            populateConfigTree();
        }
        catch (const std::exception& e)
        {
            emit errorOccurred(QString("Error refreshing config presets: %1").arg(e.what()));
        }

        QTimer::singleShot(0, this, [this, oldScrollValue]()
        {
            if (!m_configTree)
            {
                return;
            }
            if (auto* sb = m_configTree->verticalScrollBar())
            {
                sb->setValue(oldScrollValue);
            }
        });

        m_updating = false;
    }

    void ConfigPresetWidget::populateConfigTree()
    {
        const QStringList groups = m_scopeonecore->availableConfigGroups();
        for (const QString& group : groups)
        {
            QTreeWidgetItem* item = new QTreeWidgetItem(m_configTree);
            item->setText(0, group);

            const QStringList presets = m_scopeonecore->availableConfigs(group);
            const QString currentPreset = m_scopeonecore->currentConfig(group);

            auto* combo = new NoWheelComboBox(m_configTree);
            combo->addItems(presets);
            combo->setEnabled(!presets.isEmpty());

            const int currentIndex = combo->findText(currentPreset);
            if (currentIndex >= 0)
            {
                combo->setCurrentIndex(currentIndex);
            }
            else if (!currentPreset.isEmpty())
            {
                combo->addItem(currentPreset);
                combo->setCurrentText(currentPreset);
            }

            connect(combo, &QComboBox::currentTextChanged, this,
                    [this, group, combo](const QString& preset)
                    {
                        if (m_updating || preset.isEmpty())
                        {
                            return;
                        }
                        if (!m_scopeonecore->setConfig(group, preset))
                        {
                            emit errorOccurred(QString("Failed to set config %1 = %2").arg(group, preset));
                            refresh();
                            return;
                        }
                        emit configChanged(group, preset);
                    });

            m_configTree->setItemWidget(item, 1, combo);
        }
    }

    void ConfigPresetWidget::onRefreshClicked()
    {
        refresh();
    }

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

    void ConfigPresetWidget::onAutoRefreshTimer()
    {
        if (!m_updating)
        {
            refresh();
        }
    }
}

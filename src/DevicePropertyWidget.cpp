#include "DevicePropertyWidget.h"

#include "scopeone/ScopeOneCore.h"

#include <QAction>
#include <QComboBox>
#include <QDoubleValidator>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QDebug>
#include <QtGlobal>

#include <cmath>

namespace scopeone::ui
{
    namespace
    {
        // Format floating values like Micro Manager property cells
        QString formatDoubleDisplay(double value)
        {
            QString text = QString::number(value, 'f', 4);
            while (text.endsWith(QLatin1Char('0')))
            {
                text.chop(1);
            }
            if (text.endsWith(QLatin1Char('.')))
            {
                text.chop(1);
            }
            return text;
        }

        // Normalize numeric property values for display
        QString formatPropertyDisplayValue(const QString& value, bool isInteger, bool isFloat)
        {
            bool ok = false;
            const double numericValue = QLocale::c().toDouble(value.trimmed(), &ok);
            if (!ok)
            {
                return value;
            }

            return isInteger
                ? QString::number(static_cast<int>(numericValue))
                : formatDoubleDisplay(numericValue);
        }

        class NoWheelComboBox : public QComboBox
        {
        public:
            using QComboBox::QComboBox;

        protected:
            // Ignore mouse wheel edits inside the property table
            void wheelEvent(QWheelEvent* event) override
            {
                event->ignore();
            }
        };
    } // namespace

    // Create the property browser around the shared core facade
    DevicePropertyWidget::DevicePropertyWidget(scopeone::core::ScopeOneCore* core, QWidget* parent)
        : QWidget(parent)
          , m_scopeonecore(core)
          , m_showReadOnly(true)
          , m_showPreInit(false)
          , m_autoRefresh(false)
          , m_updating(false)
    {
        if (!core)
        {
            qFatal("DevicePropertyWidget requires ScopeOneCore");
        }

        setupUI();

        m_autoRefreshTimer = new QTimer(this);
        m_autoRefreshTimer->setInterval(1000);
        connect(m_autoRefreshTimer, &QTimer::timeout, this, &DevicePropertyWidget::onAutoRefreshTimer);
    }

    // Build the property table and filter controls
    void DevicePropertyWidget::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);

        auto* controlLayout = new QHBoxLayout();

        auto* refreshButton = new QPushButton("Refresh", this);
        refreshButton->setMaximumWidth(60);
        connect(refreshButton, &QPushButton::clicked, this, &DevicePropertyWidget::onRefreshClicked);

        auto* optionsButton = new QToolButton(this);
        optionsButton->setText("Options");
        optionsButton->setPopupMode(QToolButton::InstantPopup);

        auto* optionsMenu = new QMenu(optionsButton);
        auto* showReadOnlyAction = optionsMenu->addAction("Show Read-Only Properties");
        showReadOnlyAction->setCheckable(true);
        showReadOnlyAction->setChecked(m_showReadOnly);
        connect(showReadOnlyAction, &QAction::toggled, this, &DevicePropertyWidget::onShowReadOnlyToggled);

        auto* showPreInitAction = optionsMenu->addAction("Show Pre-Init Properties");
        showPreInitAction->setCheckable(true);
        showPreInitAction->setChecked(m_showPreInit);
        connect(showPreInitAction, &QAction::toggled, this, &DevicePropertyWidget::onShowPreInitToggled);

        auto* autoRefreshAction = optionsMenu->addAction("Auto Refresh");
        autoRefreshAction->setCheckable(true);
        autoRefreshAction->setChecked(m_autoRefresh);
        connect(autoRefreshAction, &QAction::toggled, this, &DevicePropertyWidget::onAutoRefreshToggled);

        optionsMenu->addSeparator();
        auto* columnsMenu = optionsMenu->addMenu("Columns");
        const auto addColumnAction = [this, columnsMenu](const QString& text, int column, bool visible)
        {
            QAction* action = columnsMenu->addAction(text);
            action->setCheckable(true);
            action->setChecked(visible);
            connect(action, &QAction::toggled, this, [this, column](bool visible)
            {
                m_propertyTree->setColumnHidden(column, !visible);
            });
        };
        addColumnAction("Value", ValueColumn, true);
        addColumnAction("Type", TypeColumn, false);
        addColumnAction("Read-Only", ReadOnlyColumn, false);
        optionsButton->setMenu(optionsMenu);

        controlLayout->addWidget(refreshButton);
        controlLayout->addWidget(optionsButton);
        controlLayout->addStretch();

        m_propertyTree = new QTreeWidget(this);
        m_propertyTree->setHeaderLabels({"Property", "Value", "Type", "Read-Only"});
        m_propertyTree->setAlternatingRowColors(true);
        m_propertyTree->setRootIsDecorated(true);
        m_propertyTree->setSortingEnabled(true);
        m_propertyTree->sortByColumn(0, Qt::AscendingOrder);
        m_propertyTree->setEditTriggers(QAbstractItemView::NoEditTriggers);

        m_propertyTree->header()->setSectionResizeMode(QHeaderView::Interactive);
        m_propertyTree->header()->resizeSection(NameColumn, 160);
        m_propertyTree->header()->resizeSection(ValueColumn, 130);
        m_propertyTree->header()->resizeSection(TypeColumn, 70);
        m_propertyTree->header()->resizeSection(ReadOnlyColumn, 70);
        m_propertyTree->setColumnHidden(TypeColumn, true);
        m_propertyTree->setColumnHidden(ReadOnlyColumn, true);

        {
            QComboBox comboProbe;
            comboProbe.setFont(m_propertyTree->font());
            QSpinBox spinProbe;
            spinProbe.setFont(m_propertyTree->font());
            QDoubleSpinBox doubleSpinProbe;
            doubleSpinProbe.setFont(m_propertyTree->font());

            const int widgetHeight = qMax(comboProbe.sizeHint().height(),
                                          qMax(spinProbe.sizeHint().height(), doubleSpinProbe.sizeHint().height()));
            const int textHeight = m_propertyTree->fontMetrics().height() + 8;
            const int rowHeight = qMax(widgetHeight, textHeight);

            m_propertyTree->setUniformRowHeights(true);
            m_propertyTree->setStyleSheet(QStringLiteral(
                    "QTreeWidget::item { height: %1px; }"
                    "QTreeWidget QComboBox, QTreeWidget QSpinBox, QTreeWidget QDoubleSpinBox, QTreeWidget QLineEdit {"
                    "  min-height: %1px; max-height: %1px; }")
                .arg(rowHeight));
        }

        mainLayout->addLayout(controlLayout);
        mainLayout->addWidget(m_propertyTree);
    }

    // Refresh values in place and rebuild only when the visible structure changes
    void DevicePropertyWidget::refresh(bool fromCache)
    {
        if (m_updating || m_scopeonecore->configurationOperationRunning())
        {
            return;
        }

        m_updating = true;
        try
        {
            if (updateExistingValues(fromCache))
            {
                m_updating = false;
                return;
            }

            const bool hadItems = m_propertyTree->topLevelItemCount() > 0;
            const int oldScrollValue = m_propertyTree->verticalScrollBar()->value();
            QSet<QString> expandedDevices;
            for (int i = 0; i < m_propertyTree->topLevelItemCount(); ++i)
            {
                QTreeWidgetItem* deviceItem = m_propertyTree->topLevelItem(i);
                if (deviceItem->isExpanded())
                {
                    expandedDevices.insert(deviceItem->text(NameColumn));
                }
            }

            QString selectedDevice;
            QString selectedProperty;
            if (QTreeWidgetItem* selectedItem = m_propertyTree->currentItem())
            {
                selectedDevice = selectedItem->data(NameColumn, Qt::UserRole).toString();
                selectedProperty = selectedItem->data(NameColumn, Qt::UserRole + 1).toString();
                if (selectedDevice.isEmpty())
                {
                    selectedDevice = selectedItem->text(NameColumn);
                }
            }

            m_propertyTree->setSortingEnabled(false);
            m_propertyTree->clear();
            populateDeviceTree(fromCache);
            m_propertyTree->setSortingEnabled(true);
            m_propertyTree->sortByColumn(NameColumn, Qt::AscendingOrder);

            for (int i = 0; i < m_propertyTree->topLevelItemCount(); ++i)
            {
                QTreeWidgetItem* deviceItem = m_propertyTree->topLevelItem(i);
                deviceItem->setExpanded(!hadItems || expandedDevices.contains(deviceItem->text(NameColumn)));
                if (deviceItem->text(NameColumn) != selectedDevice)
                {
                    continue;
                }
                if (selectedProperty.isEmpty())
                {
                    m_propertyTree->setCurrentItem(deviceItem);
                    continue;
                }
                for (int childIndex = 0; childIndex < deviceItem->childCount(); ++childIndex)
                {
                    QTreeWidgetItem* propertyItem = deviceItem->child(childIndex);
                    if (propertyItem->data(NameColumn, Qt::UserRole + 1).toString() == selectedProperty)
                    {
                        m_propertyTree->setCurrentItem(propertyItem);
                        break;
                    }
                }
            }

            QTimer::singleShot(0, this, [this, oldScrollValue]()
            {
                m_propertyTree->verticalScrollBar()->setValue(oldScrollValue);
            });
        }
        catch (const std::exception& e)
        {
            emit errorOccurred(QString("Error refreshing properties: %1").arg(e.what()));
            qWarning() << "Error refreshing properties:" << e.what();
        }
        m_updating = false;
    }

    // Add every loaded device to the property tree
    void DevicePropertyWidget::populateDeviceTree(bool fromCache)
    {
        const QStringList devices = m_scopeonecore->loadedDevices();
        for (const QString& deviceLabel : devices)
        {
            addDeviceToTree(deviceLabel, fromCache);
        }

    }

    // Update existing property editors without rebuilding the tree
    bool DevicePropertyWidget::updateExistingValues(bool fromCache)
    {
        const QStringList devices = m_scopeonecore->loadedDevices();
        if (m_propertyTree->topLevelItemCount() != devices.size())
        {
            return false;
        }

        for (const QString& deviceLabel : devices)
        {
            QTreeWidgetItem* deviceItem = nullptr;
            for (int i = 0; i < m_propertyTree->topLevelItemCount(); ++i)
            {
                QTreeWidgetItem* candidate = m_propertyTree->topLevelItem(i);
                if (candidate->text(NameColumn) == deviceLabel)
                {
                    deviceItem = candidate;
                    break;
                }
            }
            if (!deviceItem)
            {
                return false;
            }

            const auto properties = m_scopeonecore->deviceProperties(deviceLabel, fromCache);
            int visiblePropertyCount = 0;
            for (const auto& propertyInfo : properties)
            {
                if ((!m_showReadOnly && propertyInfo.isReadOnly())
                    || (!m_showPreInit && propertyInfo.isPreInit()))
                {
                    continue;
                }
                ++visiblePropertyCount;

                QTreeWidgetItem* propertyItem = nullptr;
                for (int i = 0; i < deviceItem->childCount(); ++i)
                {
                    QTreeWidgetItem* candidate = deviceItem->child(i);
                    if (candidate->data(NameColumn, Qt::UserRole + 1).toString() == propertyInfo.name())
                    {
                        propertyItem = candidate;
                        break;
                    }
                }
                if (!propertyItem)
                {
                    return false;
                }

                const QString type = propertyInfo.type();
                const QString displayType = type.isEmpty() ? QStringLiteral("Unknown") : type;
                const QString readOnlyText = propertyInfo.isReadOnly() ? QStringLiteral("Yes") : QStringLiteral("No");
                if (propertyItem->text(TypeColumn) != displayType
                    || propertyItem->text(ReadOnlyColumn) != readOnlyText)
                {
                    return false;
                }

                const bool isInteger = type == QStringLiteral("Integer");
                const bool isFloat = type == QStringLiteral("Float");
                const QStringList allowedValues = propertyInfo.allowedValues();
                const QString value = allowedValues.isEmpty() && (isInteger || isFloat)
                                          ? formatPropertyDisplayValue(propertyInfo.value(), isInteger, isFloat)
                                          : propertyInfo.value();
                QWidget* editor = m_propertyTree->itemWidget(propertyItem, ValueColumn);
                if (auto* combo = qobject_cast<QComboBox*>(editor))
                {
                    if (combo->count() != allowedValues.size())
                    {
                        return false;
                    }
                    for (int i = 0; i < allowedValues.size(); ++i)
                    {
                        if (combo->itemText(i) != allowedValues.at(i))
                        {
                            return false;
                        }
                    }
                    if (!combo->hasFocus())
                    {
                        const QSignalBlocker blocker(combo);
                        combo->setCurrentText(value);
                    }
                }
                else if (auto* lineEdit = qobject_cast<QLineEdit*>(editor))
                {
                    if (!allowedValues.isEmpty() || (!isInteger && !isFloat))
                    {
                        return false;
                    }
                    if (!lineEdit->hasFocus())
                    {
                        const QSignalBlocker blocker(lineEdit);
                        lineEdit->setText(value);
                    }
                }
                else
                {
                    const bool needsEditor = !propertyInfo.isReadOnly()
                        && (!allowedValues.isEmpty() || isInteger || isFloat);
                    if (needsEditor)
                    {
                        return false;
                    }
                    propertyItem->setText(ValueColumn, value);
                }
            }
            if (deviceItem->childCount() != visiblePropertyCount)
            {
                return false;
            }
        }
        return true;
    }

    // Submit a property value and read back the actual accepted value
    bool DevicePropertyWidget::applyPropertyValue(const QString& deviceLabel,
                                                  const QString& propertyName,
                                                  const QString& requestedValue,
                                                  QString& actualValue)
    {
        QString error;
        if (!m_scopeonecore->setPropertyValue(deviceLabel, propertyName, requestedValue, &error))
        {
            emit errorOccurred(QString("Failed to set property %1.%2: %3")
                .arg(deviceLabel, propertyName, error));
            return false;
        }

        actualValue = m_scopeonecore->getPropertyValue(deviceLabel, propertyName, false);
        return true;
    }

    // Add one device node and its properties to the tree
    void DevicePropertyWidget::addDeviceToTree(const QString& deviceLabel, bool fromCache)
    {
        try
        {
            QTreeWidgetItem* deviceItem = new QTreeWidgetItem(m_propertyTree);
            deviceItem->setText(NameColumn, deviceLabel);
            deviceItem->setText(ValueColumn, "");
            deviceItem->setText(TypeColumn, "Device");
            deviceItem->setText(ReadOnlyColumn, "");
            deviceItem->setData(NameColumn, Qt::UserRole, deviceLabel);

            QFont font = deviceItem->font(NameColumn);
            font.setBold(true);
            deviceItem->setFont(NameColumn, font);

            const auto properties = m_scopeonecore->deviceProperties(deviceLabel, fromCache);
            for (const auto& propertyInfo : properties)
            {
                addPropertyToDevice(deviceItem, deviceLabel, propertyInfo);
            }
        }
        catch (const std::exception& e)
        {
            emit errorOccurred(QString("Error adding device %1: %2").arg(deviceLabel, e.what()));
        }
    }

    // Create the correct editor for one device property
    void DevicePropertyWidget::addPropertyToDevice(
        QTreeWidgetItem* deviceItem,
        const QString& deviceLabel,
        const scopeone::core::ScopeOneCore::DevicePropertyInfo& propertyInfo)
    {
        try
        {
            const QString propertyName = propertyInfo.name();
            const bool isReadOnly = propertyInfo.isReadOnly();
            const bool isPreInit = propertyInfo.isPreInit();

            if (!m_showReadOnly && isReadOnly) return;
            if (!m_showPreInit && isPreInit) return;

            QTreeWidgetItem* propertyItem = new QTreeWidgetItem(deviceItem);
            propertyItem->setText(NameColumn, propertyName);

            const QString typeStr = propertyInfo.type();
            const bool isInteger = (typeStr == "Integer");
            const bool isFloat = (typeStr == "Float");
            const QStringList allowedValues = propertyInfo.allowedValues();
            const QString rawValue = propertyInfo.value();
            const QString value = allowedValues.isEmpty() && (isInteger || isFloat)
                ? formatPropertyDisplayValue(rawValue, isInteger, isFloat)
                : rawValue;

            propertyItem->setText(ValueColumn, value);
            propertyItem->setText(TypeColumn, typeStr.isEmpty() ? QStringLiteral("Unknown") : typeStr);
            propertyItem->setText(ReadOnlyColumn, isReadOnly ? "Yes" : "No");

            Qt::ItemFlags flags = propertyItem->flags();
            flags |= Qt::ItemIsSelectable | Qt::ItemIsEnabled;

            if (isReadOnly)
            {
                flags &= ~Qt::ItemIsEditable;
                for (int col = 0; col < m_propertyTree->columnCount(); ++col)
                {
                    propertyItem->setForeground(col, QColor(128, 128, 128));
                }
            }
            else
            {
                flags &= ~Qt::ItemIsEditable;
                propertyItem->setForeground(ValueColumn, QColor(0, 0, 200));

                QWidget* editor = nullptr;

                if (!allowedValues.isEmpty())
                {
                    QComboBox* combo = new NoWheelComboBox();
                    for (const QString& v : allowedValues)
                    {
                        combo->addItem(v);
                    }
                    combo->setCurrentText(value);

                    connect(combo, &QComboBox::currentTextChanged, this,
                            [this, combo, deviceLabel, propertyName](const QString& newValue)
                            {
                                QString actualValue;
                                if (!applyPropertyValue(deviceLabel, propertyName, newValue, actualValue))
                                {
                                    return;
                                }
                                {
                                    QSignalBlocker blocker(combo);
                                    combo->setCurrentText(actualValue);
                                }
                                emit propertyChanged(deviceLabel, propertyName, actualValue);
                            });

                    editor = combo;
                }

                if (!editor && (isInteger || isFloat))
                {
                    QLineEdit* lineEdit = new QLineEdit();
                    lineEdit->setText(value);
                    if (isInteger)
                    {
                        auto* validator = new QIntValidator(lineEdit);
                        validator->setLocale(QLocale::c());
                        lineEdit->setValidator(validator);
                    }
                    else
                    {
                        auto* validator = new QDoubleValidator(lineEdit);
                        validator->setLocale(QLocale::c());
                        validator->setNotation(QDoubleValidator::StandardNotation);
                        validator->setDecimals(16);
                        lineEdit->setValidator(validator);
                    }

                    connect(lineEdit, &QLineEdit::editingFinished, this,
                            [this,
                             lineEdit,
                             deviceLabel,
                             propertyName,
                             isInteger,
                             isFloat,
                             hasLimits = propertyInfo.hasLimits(),
                             lowerLimit = propertyInfo.lowerLimit(),
                             upperLimit = propertyInfo.upperLimit()]()
                            {
                                QString newValue = lineEdit->text().trimmed();
                                bool ok = false;
                                double numericValue = newValue.toDouble(&ok);
                                if (!ok)
                                {
                                    return;
                                }
                                if (hasLimits)
                                {
                                    const double lower = isInteger ? std::ceil(lowerLimit) : lowerLimit;
                                    const double upper = isInteger ? std::floor(upperLimit) : upperLimit;
                                    numericValue = qBound(lower, numericValue, upper);
                                }
                                newValue = isInteger
                                    ? QString::number(static_cast<int>(numericValue))
                                    : QString::number(numericValue, 'f', 4);

                                QString actualValue;
                                if (!applyPropertyValue(deviceLabel, propertyName, newValue, actualValue))
                                {
                                    return;
                                }
                                actualValue = formatPropertyDisplayValue(actualValue, isInteger, isFloat);
                                {
                                    QSignalBlocker blocker(lineEdit);
                                    lineEdit->setText(actualValue);
                                }
                                emit propertyChanged(deviceLabel, propertyName, actualValue);
                            });

                    editor = lineEdit;
                }

                if (editor)
                {
                    m_propertyTree->setItemWidget(propertyItem, ValueColumn, editor);
                }
            }

            propertyItem->setFlags(flags);
            propertyItem->setData(NameColumn, Qt::UserRole, deviceLabel);
            propertyItem->setData(NameColumn, Qt::UserRole + 1, propertyName);
        }
        catch (const std::exception& e)
        {
            emit errorOccurred(QString("Error adding property %1.%2: %3")
                .arg(deviceLabel, propertyInfo.name(), e.what()));
        }
    }

    // Refresh all property values from hardware
    void DevicePropertyWidget::onRefreshClicked()
    {
        refresh(false);
    }

    // Toggle read only property visibility
    void DevicePropertyWidget::onShowReadOnlyToggled(bool show)
    {
        m_showReadOnly = show;
        refresh(true);
    }

    // Toggle pre init property visibility
    void DevicePropertyWidget::onShowPreInitToggled(bool show)
    {
        m_showPreInit = show;
        refresh(true);
    }

    // Enable or disable cached periodic refresh
    void DevicePropertyWidget::onAutoRefreshToggled(bool enabled)
    {
        m_autoRefresh = enabled;
        if (enabled)
        {
            m_autoRefreshTimer->start();
        }
        else
        {
            m_autoRefreshTimer->stop();
        }
    }

    // Refresh cached values when the timer fires
    void DevicePropertyWidget::onAutoRefreshTimer()
    {
        if (!m_updating)
        {
            refresh(true);
        }
    }
} // namespace scopeone::ui

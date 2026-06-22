#include "DevicePropertyWidget.h"

#include "scopeone/ScopeOneCore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleValidator>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QTimer>
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
        if (!m_scopeonecore)
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

        auto* showReadOnlyCheckBox = new QCheckBox("Show Read-Only", this);
        showReadOnlyCheckBox->setChecked(m_showReadOnly);
        connect(showReadOnlyCheckBox, &QCheckBox::toggled, this, &DevicePropertyWidget::onShowReadOnlyToggled);

        auto* showPreInitCheckBox = new QCheckBox("Show Pre-Init", this);
        showPreInitCheckBox->setChecked(m_showPreInit);
        connect(showPreInitCheckBox, &QCheckBox::toggled, this, &DevicePropertyWidget::onShowPreInitToggled);

        auto* autoRefreshCheckBox = new QCheckBox("Auto Refresh", this);
        autoRefreshCheckBox->setChecked(m_autoRefresh);
        connect(autoRefreshCheckBox, &QCheckBox::toggled, this, &DevicePropertyWidget::onAutoRefreshToggled);

        controlLayout->addWidget(refreshButton);
        controlLayout->addWidget(showReadOnlyCheckBox);
        controlLayout->addWidget(showPreInitCheckBox);
        controlLayout->addWidget(autoRefreshCheckBox);
        controlLayout->addStretch();

        m_propertyTree = new QTreeWidget(this);
        m_propertyTree->setHeaderLabels({"Property", "Value", "Type", "Read-Only"});
        m_propertyTree->setAlternatingRowColors(true);
        m_propertyTree->setRootIsDecorated(true);
        m_propertyTree->setSortingEnabled(true);
        m_propertyTree->sortByColumn(0, Qt::AscendingOrder);
        m_propertyTree->setEditTriggers(QAbstractItemView::NoEditTriggers);

        m_propertyTree->header()->resizeSection(NameColumn, 200);
        m_propertyTree->header()->resizeSection(ValueColumn, 150);
        m_propertyTree->header()->resizeSection(TypeColumn, 70);
        m_propertyTree->header()->resizeSection(ReadOnlyColumn, 70);

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

    // Rebuild visible properties while preserving scroll position
    void DevicePropertyWidget::refresh(bool fromCache)
    {
        // Rebuild the tree and keep the scroll position
        if (m_updating)
        {
            return;
        }

        const int oldScrollValue =
            m_propertyTree->verticalScrollBar() ? m_propertyTree->verticalScrollBar()->value() : 0;

        m_updating = true;
        m_propertyTree->clear();

        try
        {
            populateDeviceTree(fromCache);
        }
        catch (const std::exception& e)
        {
            emit errorOccurred(QString("Error refreshing properties: %1").arg(e.what()));
            qDebug() << "Error refreshing properties:" << e.what();
        }

        QTimer::singleShot(0, this, [this, oldScrollValue]()
        {
            if (auto* sb = m_propertyTree->verticalScrollBar())
            {
                sb->setValue(oldScrollValue);
            }
        });

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

        m_propertyTree->expandAll();
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
        if (actualValue.isEmpty())
        {
            actualValue = requestedValue;
        }
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
                                combo->blockSignals(true);
                                combo->setCurrentText(actualValue);
                                combo->blockSignals(false);
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
                                lineEdit->blockSignals(true);
                                lineEdit->setText(actualValue);
                                lineEdit->blockSignals(false);
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

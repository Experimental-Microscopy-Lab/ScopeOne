#pragma once

#include "scopeone/ScopeOneCore.h"

#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;
class QTimer;

namespace scopeone::ui {

class DevicePropertyWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DevicePropertyWidget(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);
    ~DevicePropertyWidget() override = default;

    void refresh(bool fromCache = false);

signals:
    void propertyChanged(const QString& device, const QString& property, const QString& value);
    void errorOccurred(const QString& message);

private:
    void onRefreshClicked();
    void onShowReadOnlyToggled(bool show);
    void onShowPreInitToggled(bool show);
    void onAutoRefreshToggled(bool enabled);
    void onAutoRefreshTimer();

    void setupUI();
    void populateDeviceTree(bool fromCache);
    void addDeviceToTree(const QString& deviceLabel, bool fromCache);
    void addPropertyToDevice(
        QTreeWidgetItem* deviceItem,
        const QString& deviceLabel,
        const scopeone::core::ScopeOneCore::DevicePropertyInfo& propertyInfo);

    scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};

    QTreeWidget* m_propertyTree{nullptr};
    QTimer* m_autoRefreshTimer{nullptr};

    bool m_showReadOnly;
    bool m_showPreInit;
    bool m_autoRefresh;
    bool m_updating;
    enum TreeColumns {
        NameColumn = 0,
        ValueColumn = 1,
        TypeColumn = 2,
        ReadOnlyColumn = 3
    };
};

}

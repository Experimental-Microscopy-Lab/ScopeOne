#pragma once

#include <QWidget>

class QTimer;
class QTreeWidget;

namespace scopeone::core { class ScopeOneCore; }

namespace scopeone::ui {

class ConfigPresetWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigPresetWidget(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);
    ~ConfigPresetWidget() override = default;

    void refresh();

signals:
    void configChanged(const QString& group, const QString& preset);
    void errorOccurred(const QString& message);

private:
    void onRefreshClicked();
    void onAutoRefreshToggled(bool enabled);
    void onAutoRefreshTimer();

    void setupUI();
    void populateConfigTree();

    scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
    QTreeWidget* m_configTree{nullptr};
    QTimer* m_autoRefreshTimer{nullptr};
    bool m_autoRefresh{false};
    bool m_updating{false};
};

}

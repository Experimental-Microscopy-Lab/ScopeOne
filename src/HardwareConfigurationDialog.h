#pragma once

#include <QDialog>
#include <QList>

#include <utility>
#include "scopeone/HardwareConfiguration.h"

class QListWidget;
class QStackedWidget;
class QTreeWidget;
class QComboBox;

namespace scopeone::core { class ScopeOneCore; }

namespace scopeone::ui
{
    class HardwareConfigurationDialog : public QDialog
    {
    public:
        explicit HardwareConfigurationDialog(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);
        void showRoles();
        QString configurationPath() const { return m_path; }
        // Whether the devices changed, so the written configuration has to be loaded again
        bool reloadNeeded() const { return m_reload; }

    private:
        void refreshDevices();
        void addDevice();
        void editDevice();
        void removeDevice();
        void readRoles();
        void refreshLightSources();
        void editLightSource(bool existing);
        QString changes() const;
        void apply();

        scopeone::core::ScopeOneCore* m_core;
        scopeone::core::HardwareConfiguration m_document;
        QByteArray m_original;
        QString m_path;
        QListWidget* m_steps;
        QStackedWidget* m_pages;
        QTreeWidget* m_devices;
        // The Core roles with the combo that picks each
        QList<std::pair<QString, QComboBox*>> m_roles;
        QListWidget* m_lightSources;
        bool m_reload{false};
    };
}

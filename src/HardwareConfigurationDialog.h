#pragma once

#include <QDialog>
#include <QList>

#include <utility>
#include "scopeone/HardwareConfiguration.h"

class QListWidget;
class QStackedWidget;
class QTableWidget;
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
        // Takes the rows of the Limits page into the document, false when a row is not a valid range
        bool readLimits();
        QString changes() const;
        void apply();

        scopeone::core::ScopeOneCore* m_core;
        scopeone::core::HardwareConfiguration m_document;
        QByteArray m_original;
        QString m_path;
        QListWidget* m_steps;
        QStackedWidget* m_pages;
        QTreeWidget* m_devices;
        // The roles with the combo that picks each
        QList<std::pair<QString, QComboBox*>> m_roles;
        QListWidget* m_lightSources;
        QTableWidget* m_limits;
        bool m_reload{false};
    };
}

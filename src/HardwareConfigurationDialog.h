#pragma once

#include <QDialog>
#include "scopeone/HardwareConfiguration.h"

class QListWidget;
class QStackedWidget;
class QTreeWidget;
class QPlainTextEdit;
class QComboBox;

namespace scopeone::core { class ScopeOneCore; }

namespace scopeone::ui
{
    class HardwareConfigurationDialog : public QDialog
    {
    public:
        explicit HardwareConfigurationDialog(scopeone::core::ScopeOneCore* core, QWidget* parent = nullptr);
        void showLightSources();
        QString configurationPath() const { return m_path; }

    private:
        void refreshDevices();
        void addDevice();
        void editDevice();
        void removeDevice();
        bool updateChanges();
        void apply();

        scopeone::core::ScopeOneCore* m_core;
        scopeone::core::HardwareConfiguration m_document;
        QByteArray m_original;
        QString m_path;
        QListWidget* m_steps;
        QStackedWidget* m_pages;
        QTreeWidget* m_devices;
        QPlainTextEdit* m_changes;
        QComboBox* m_camera;
        QComboBox* m_shutter;
        QComboBox* m_focus;
        QComboBox* m_autoShutter;
    };
}

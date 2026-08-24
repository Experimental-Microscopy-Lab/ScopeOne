#pragma once

#include <QDialog>
#include <QStringList>

class QPlainTextEdit;
class QTableWidget;

namespace scopeone::core
{
    class ScopeOneCore;
}

namespace scopeone::ui
{
    QStringList loadConfiguredHardwarePlugins(scopeone::core::ScopeOneCore& core);

    class PluginManagerDialog final : public QDialog
    {
        Q_OBJECT

    public:
        explicit PluginManagerDialog(QWidget* parent = nullptr);

    private:
        void refreshPlugins();
        void installPlugin();
        void showSelectedOptions();
        void saveHardwareSettings();

        QTableWidget* m_table{nullptr};
        QPlainTextEdit* m_optionsEdit{nullptr};
    };
}

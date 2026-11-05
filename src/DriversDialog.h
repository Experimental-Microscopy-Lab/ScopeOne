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
    QStringList loadConfiguredDrivers(scopeone::core::ScopeOneCore& core);

    class DriversDialog final : public QDialog
    {
        Q_OBJECT

    public:
        explicit DriversDialog(QWidget* parent = nullptr);

    private:
        void refreshDrivers();
        void installDriver();
        void showSelectedOptions();
        void saveSettings();

        QTableWidget* m_table{nullptr};
        QPlainTextEdit* m_detailsEdit{nullptr};
        QPlainTextEdit* m_optionsEdit{nullptr};
    };
}

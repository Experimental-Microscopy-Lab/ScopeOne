#pragma once

#include <QDialog>

class QPlainTextEdit;
class QTableWidget;

namespace scopeone::ui
{
    class DriversDialog final : public QDialog
    {
        Q_OBJECT

    public:
        explicit DriversDialog(QWidget* parent = nullptr);

    private:
        void refreshDrivers();
        void installDriver();
        void installDescription();
        void showSelectedDetails();

        QTableWidget* m_table{nullptr};
        QPlainTextEdit* m_detailsEdit{nullptr};
    };
}

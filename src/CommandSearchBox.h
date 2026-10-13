#pragma once

#include <QLineEdit>
#include <QList>
#include <QString>

class QAction;
class QListWidget;
class QMenuBar;

namespace scopeone::ui
{
    // A search field for the commands of all menus, which sits at the right end of the menu bar.
    // Typing lists the matching commands under the field, Up and Down move through them and
    // Enter runs the chosen one.
    class CommandSearchBox : public QLineEdit
    {
        Q_OBJECT

    public:
        explicit CommandSearchBox(QMenuBar* menuBar, QWidget* parent = nullptr);

        // Puts the cursor in the field, ready for typing
        void activate();

    protected:
        void keyPressEvent(QKeyEvent* event) override;
        void focusInEvent(QFocusEvent* event) override;
        void focusOutEvent(QFocusEvent* event) override;

    private:
        struct Command
        {
            QAction* action{nullptr};
            QString name;
            QString path;
            QString shortcut;
        };
        void collect();
        void collectMenu(const QList<QAction*>& actions, const QString& path);
        void refresh();
        void showPopup();
        void run(int row);

        QMenuBar* m_menuBar{nullptr};
        QList<Command> m_commands;
        QList<int> m_shown;
        QListWidget* m_popup{nullptr};
    };
}

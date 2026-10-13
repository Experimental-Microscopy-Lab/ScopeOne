#pragma once

#include <QDialog>
#include <QList>
#include <QString>

class QAction;
class QLineEdit;
class QListWidget;
class QMenuBar;

namespace scopeone::ui
{
    // Finds a menu command by typing a part of its name, like Find Commands in ImageJ. The list
    // follows what is typed, and Enter runs the chosen command.
    class CommandFinder : public QDialog
    {
        Q_OBJECT

    public:
        explicit CommandFinder(QMenuBar* menuBar, QWidget* parent = nullptr);

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        struct Command
        {
            QAction* action{nullptr};
            QString name;
            QString path;
            QString shortcut;
        };
        void collect(QMenuBar* menuBar);
        void collectMenu(const QList<QAction*>& actions, const QString& path);
        void refresh();
        void run(int row);

        QList<Command> m_commands;
        QList<int> m_shown;
        QLineEdit* m_input{nullptr};
        QListWidget* m_list{nullptr};
    };
}

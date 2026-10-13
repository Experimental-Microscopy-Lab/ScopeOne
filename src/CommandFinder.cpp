#include "CommandFinder.h"

#include <QAction>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QVBoxLayout>

#include <algorithm>

namespace scopeone::ui
{
    namespace
    {
        // The text of a menu entry as it reads, without the mnemonic marks and the dots of a dialog
        QString plain(QString text)
        {
            text.remove(QLatin1Char('&'));
            while (text.endsWith(QLatin1Char('.')) || text.endsWith(QChar(0x2026)))
            {
                text.chop(1);
            }
            return text.trimmed();
        }
    }

    CommandFinder::CommandFinder(QMenuBar* menuBar, QWidget* parent)
        : QDialog(parent)
    {
        setWindowTitle(tr("Find Commands"));
        resize(560, 400);
        auto* layout = new QVBoxLayout(this);
        m_input = new QLineEdit(this);
        m_input->setPlaceholderText(tr("Type part of a command name"));
        m_input->setClearButtonEnabled(true);
        m_list = new QListWidget(this);
        layout->addWidget(m_input);
        layout->addWidget(m_list, 1);

        m_input->installEventFilter(this);
        connect(m_input, &QLineEdit::textChanged, this, [this]() { refresh(); });
        connect(m_input, &QLineEdit::returnPressed, this, [this]() { run(m_list->currentRow()); });
        connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem*) { run(m_list->currentRow()); });

        collect(menuBar);
        refresh();
        m_input->setFocus();
    }

    void CommandFinder::collect(QMenuBar* menuBar)
    {
        collectMenu(menuBar->actions(), QString());
        std::sort(m_commands.begin(), m_commands.end(), [](const Command& a, const Command& b)
        {
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });
    }

    void CommandFinder::collectMenu(const QList<QAction*>& actions, const QString& path)
    {
        for (QAction* action : actions)
        {
            if (action->isSeparator())
            {
                continue;
            }
            if (QMenu* submenu = action->menu())
            {
                const QString title = plain(submenu->title().isEmpty() ? action->text() : submenu->title());
                collectMenu(submenu->actions(), path.isEmpty() ? title : path + QStringLiteral(" > ") + title);
                continue;
            }
            const QString name = plain(action->text());
            if (name.isEmpty())
            {
                continue;
            }
            m_commands.append({action, name, path, action->shortcut().toString(QKeySequence::NativeText)});
        }
    }

    // Shows the commands that hold every word that was typed, those that begin with it first
    void CommandFinder::refresh()
    {
        const QStringList words = m_input->text().toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QList<QPair<int, int>> matches;
        for (int index = 0; index < m_commands.size(); ++index)
        {
            const Command& command = m_commands.at(index);
            const QString name = command.name.toLower();
            const QString full = name + QLatin1Char(' ') + command.path.toLower();
            bool all = true;
            for (const QString& word : words)
            {
                all = all && full.contains(word);
            }
            if (!all)
            {
                continue;
            }
            const int rank = words.isEmpty() ? 2 : (name.startsWith(words.first()) ? 0 : (name.contains(words.first()) ? 1 : 2));
            matches.append({rank, index});
        }
        std::stable_sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

        m_list->clear();
        m_shown.clear();
        for (const auto& match : matches)
        {
            const Command& command = m_commands.at(match.second);
            QString text = command.name;
            if (!command.path.isEmpty())
            {
                text += QStringLiteral("      ") + command.path;
            }
            if (!command.shortcut.isEmpty())
            {
                text += QStringLiteral("      [") + command.shortcut + QLatin1Char(']');
            }
            auto* item = new QListWidgetItem(text, m_list);
            item->setFlags(command.action->isEnabled() ? item->flags() : (item->flags() & ~Qt::ItemIsEnabled));
            m_shown.append(match.second);
            if (m_shown.size() >= 200)
            {
                break;
            }
        }
        if (m_list->count() > 0)
        {
            m_list->setCurrentRow(0);
        }
    }

    void CommandFinder::run(int row)
    {
        if (row < 0 || row >= m_shown.size())
        {
            return;
        }
        QAction* action = m_commands.at(m_shown.at(row)).action;
        // The dialog goes first so that the command opens its own window on top of the main window
        accept();
        action->trigger();
    }

    bool CommandFinder::eventFilter(QObject* watched, QEvent* event)
    {
        if (watched == m_input && event->type() == QEvent::KeyPress)
        {
            const int key = static_cast<QKeyEvent*>(event)->key();
            if (key == Qt::Key_Down || key == Qt::Key_Up)
            {
                const int next = m_list->currentRow() + (key == Qt::Key_Down ? 1 : -1);
                m_list->setCurrentRow(std::clamp(next, 0, std::max(0, m_list->count() - 1)));
                return true;
            }
        }
        return QDialog::eventFilter(watched, event);
    }
}

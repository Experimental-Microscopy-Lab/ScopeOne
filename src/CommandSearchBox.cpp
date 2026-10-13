#include "CommandSearchBox.h"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QScreen>

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

    CommandSearchBox::CommandSearchBox(QMenuBar* menuBar, QWidget* parent)
        : QLineEdit(parent)
        , m_menuBar(menuBar)
    {
        setPlaceholderText(tr("Search commands (L)"));
        setClearButtonEnabled(true);
        setFixedWidth(240);

        // The list takes no focus, so that typing goes on in the field
        m_popup = new QListWidget(nullptr);
        m_popup->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
        m_popup->setAttribute(Qt::WA_ShowWithoutActivating);
        m_popup->setFocusPolicy(Qt::NoFocus);
        m_popup->setFrameShape(QFrame::StyledPanel);
        connect(m_popup, &QListWidget::itemClicked, this, [this](QListWidgetItem*)
        {
            run(m_popup->currentRow());
        });
        connect(this, &QLineEdit::textChanged, this, [this]() { refresh(); });
        connect(this, &QObject::destroyed, m_popup, &QObject::deleteLater);
    }

    void CommandSearchBox::activate()
    {
        setFocus();
        selectAll();
    }

    void CommandSearchBox::collect()
    {
        m_commands.clear();
        collectMenu(m_menuBar->actions(), QString());
        std::sort(m_commands.begin(), m_commands.end(), [](const Command& a, const Command& b)
        {
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });
    }

    void CommandSearchBox::collectMenu(const QList<QAction*>& actions, const QString& path)
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
            if (!name.isEmpty())
            {
                m_commands.append({action, name, path, action->shortcut().toString(QKeySequence::NativeText)});
            }
        }
    }

    // Lists the commands that hold every word that was typed, those that begin with it first
    void CommandSearchBox::refresh()
    {
        const QStringList words = text().toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        m_popup->clear();
        m_shown.clear();
        if (words.isEmpty())
        {
            m_popup->hide();
            return;
        }
        // The menus change while the application runs, so the commands are read again for each search
        if (m_commands.isEmpty() || words.size() == 1)
        {
            collect();
        }
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
            if (all)
            {
                matches.append({name.startsWith(words.first()) ? 0 : (name.contains(words.first()) ? 1 : 2), index});
            }
        }
        std::stable_sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& match : matches)
        {
            const Command& command = m_commands.at(match.second);
            QString label = command.name;
            if (!command.path.isEmpty())
            {
                label += QStringLiteral("      ") + command.path;
            }
            if (!command.shortcut.isEmpty())
            {
                label += QStringLiteral("      [") + command.shortcut + QLatin1Char(']');
            }
            auto* item = new QListWidgetItem(label, m_popup);
            item->setFlags(command.action->isEnabled() ? item->flags() : (item->flags() & ~Qt::ItemIsEnabled));
            m_shown.append(match.second);
            if (m_shown.size() >= 30)
            {
                break;
            }
        }
        if (m_popup->count() == 0)
        {
            m_popup->hide();
            return;
        }
        m_popup->setCurrentRow(0);
        showPopup();
    }

    // Puts the list under the field, kept inside the screen
    void CommandSearchBox::showPopup()
    {
        const int rows = std::min(m_popup->count(), 12);
        const int popupWidth = 520;
        const int popupHeight = rows * m_popup->sizeHintForRow(0) + 6;
        const QPoint below = mapToGlobal(QPoint(width(), height()));
        QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
        if (const QScreen* current = window()->screen())
        {
            screen = current->availableGeometry();
        }
        const int x = std::clamp(below.x() - popupWidth, screen.left(), screen.right() - popupWidth);
        m_popup->setGeometry(x, below.y(), popupWidth, popupHeight);
        m_popup->show();
    }

    void CommandSearchBox::run(int row)
    {
        if (row < 0 || row >= m_shown.size())
        {
            return;
        }
        QAction* action = m_commands.at(m_shown.at(row)).action;
        m_popup->hide();
        clear();
        clearFocus();
        action->trigger();
    }

    void CommandSearchBox::keyPressEvent(QKeyEvent* event)
    {
        if (m_popup->isVisible())
        {
            switch (event->key())
            {
            case Qt::Key_Down:
            case Qt::Key_Up:
            {
                const int next = m_popup->currentRow() + (event->key() == Qt::Key_Down ? 1 : -1);
                m_popup->setCurrentRow(std::clamp(next, 0, m_popup->count() - 1));
                return;
            }
            case Qt::Key_Return:
            case Qt::Key_Enter:
                run(m_popup->currentRow());
                return;
            default:
                break;
            }
        }
        if (event->key() == Qt::Key_Escape)
        {
            m_popup->hide();
            clear();
            clearFocus();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

    void CommandSearchBox::focusInEvent(QFocusEvent* event)
    {
        QLineEdit::focusInEvent(event);
        refresh();
    }

    void CommandSearchBox::focusOutEvent(QFocusEvent* event)
    {
        QLineEdit::focusOutEvent(event);
        m_popup->hide();
    }
}

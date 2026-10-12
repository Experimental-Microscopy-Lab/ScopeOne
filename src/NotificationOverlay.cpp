#include "NotificationOverlay.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace scopeone::ui
{
    namespace
    {
        constexpr int kMaximumShown = 4;
        constexpr int kOverlayWidth = 380;
        constexpr int kMargin = 12;
    }

    NotificationOverlay::NotificationOverlay(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(6);
        setFixedWidth(kOverlayWidth);
        hide();
        parent->installEventFilter(this);
    }

    // Shows a message at the bottom of the stack. Errors stay longer than the others.
    void NotificationOverlay::show(Level level, const QString& title, const QString& message)
    {
        // The oldest message makes room when there are too many
        while (m_layout->count() >= kMaximumShown)
        {
            if (QLayoutItem* item = m_layout->takeAt(0))
            {
                if (QWidget* widget = item->widget())
                {
                    widget->deleteLater();
                }
                delete item;
            }
        }

        const QString accent = level == Level::Error ? QStringLiteral("#c0392b")
                             : level == Level::Warning ? QStringLiteral("#d68910")
                                                       : QStringLiteral("#2e86c1");
        auto* toast = new QFrame(this);
        toast->setObjectName(QStringLiteral("notificationToast"));
        toast->setStyleSheet(QStringLiteral(
            "#notificationToast { background: palette(window); border: 1px solid palette(mid);"
            " border-left: 5px solid %1; border-radius: 4px; }").arg(accent));
        auto* row = new QHBoxLayout(toast);
        row->setContentsMargins(10, 8, 6, 8);

        auto* text = new QVBoxLayout();
        text->setSpacing(2);
        auto* titleLabel = new QLabel(title, toast);
        QFont font = titleLabel->font();
        font.setBold(true);
        titleLabel->setFont(font);
        auto* messageLabel = new QLabel(message, toast);
        messageLabel->setWordWrap(true);
        messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        text->addWidget(titleLabel);
        text->addWidget(messageLabel);
        row->addLayout(text, 1);

        auto* close = new QToolButton(toast);
        close->setText(QStringLiteral("✕"));
        close->setAutoRaise(true);
        close->setToolTip(tr("Dismiss"));
        row->addWidget(close, 0, Qt::AlignTop);

        auto dismiss = [this, toast]()
        {
            m_layout->removeWidget(toast);
            toast->deleteLater();
            QTimer::singleShot(0, this, [this]()
            {
                if (m_layout->count() == 0)
                {
                    hide();
                }
                else
                {
                    reposition();
                }
            });
        };
        connect(close, &QToolButton::clicked, toast, dismiss);
        QTimer::singleShot(level == Level::Error ? 20000 : 8000, toast, dismiss);

        m_layout->addWidget(toast);
        QWidget::show();
        raise();
        reposition();
    }

    void NotificationOverlay::reposition()
    {
        QWidget* host = parentWidget();
        if (!host)
        {
            return;
        }
        adjustSize();
        // Above the status bar of a main window
        move(host->width() - width() - kMargin, host->height() - height() - kMargin - 28);
        raise();
    }

    bool NotificationOverlay::eventFilter(QObject* watched, QEvent* event)
    {
        if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible())
        {
            reposition();
        }
        return QWidget::eventFilter(watched, event);
    }
}

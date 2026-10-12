#pragma once

#include <QString>
#include <QWidget>

class QVBoxLayout;

namespace scopeone::ui
{
    // Messages that appear over the corner of a window without taking the focus or blocking
    // anything, unlike a message box. They go away on their own or when closed.
    class NotificationOverlay : public QWidget
    {
        Q_OBJECT

    public:
        enum class Level
        {
            Info,
            Warning,
            Error
        };

        explicit NotificationOverlay(QWidget* parent);

        void show(Level level, const QString& title, const QString& message);
        // Moves the overlay to the lower right corner of the parent
        void reposition();

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        QVBoxLayout* m_layout{nullptr};
    };
}

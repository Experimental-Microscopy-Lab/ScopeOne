#pragma once

#include <QObject>
#include <QString>

class QAbstractItemView;
class QLabel;

namespace scopeone::ui
{
    // A few words over a list or table while it has no rows, that say what puts something in it
    class EmptyHint : public QObject
    {
        Q_OBJECT

    public:
        EmptyHint(QAbstractItemView* view, const QString& text);

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        void update();

        QAbstractItemView* m_view{nullptr};
        QLabel* m_label{nullptr};
    };
}

#include "EmptyHint.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QEvent>
#include <QLabel>

namespace scopeone::ui
{
    EmptyHint::EmptyHint(QAbstractItemView* view, const QString& text)
        : QObject(view), m_view(view)
    {
        m_label = new QLabel(text, view->viewport());
        m_label->setAlignment(Qt::AlignCenter);
        m_label->setWordWrap(true);
        m_label->setEnabled(false);
        m_label->setAttribute(Qt::WA_TransparentForMouseEvents);
        view->viewport()->installEventFilter(this);
        QAbstractItemModel* model = view->model();
        connect(model, &QAbstractItemModel::rowsInserted, this, &EmptyHint::update);
        connect(model, &QAbstractItemModel::rowsRemoved, this, &EmptyHint::update);
        connect(model, &QAbstractItemModel::modelReset, this, &EmptyHint::update);
        connect(model, &QAbstractItemModel::layoutChanged, this, &EmptyHint::update);
        update();
    }

    bool EmptyHint::eventFilter(QObject* watched, QEvent* event)
    {
        if (watched == m_view->viewport() && event->type() == QEvent::Resize)
        {
            update();
        }
        return QObject::eventFilter(watched, event);
    }

    void EmptyHint::update()
    {
        m_label->setGeometry(m_view->viewport()->rect().adjusted(12, 12, -12, -12));
        m_label->setVisible(m_view->model()->rowCount() == 0);
    }
}

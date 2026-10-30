#pragma once

#include <QFrame>
#include <QString>

class QFormLayout;
class QToolButton;
class QWidget;

namespace scopeone::ui
{
    // A titled block of a form that can be folded away. The title can carry a short summary of
    // the settings inside, so a folded block still tells what it is set to.
    class CollapsibleSection : public QFrame
    {
        Q_OBJECT

    public:
        explicit CollapsibleSection(const QString& title, QWidget* parent = nullptr);

        // The form that the settings of the block are added to
        QFormLayout* form() const { return m_form; }
        void setSummary(const QString& summary);
        void setExpanded(bool expanded);
        bool isExpanded() const;

    private:
        void updateTitle();

        QToolButton* m_header{nullptr};
        QWidget* m_body{nullptr};
        QFormLayout* m_form{nullptr};
        QString m_title;
        QString m_summary;
    };
}

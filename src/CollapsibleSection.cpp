#include "CollapsibleSection.h"

#include <QFormLayout>
#include <QToolButton>
#include <QVBoxLayout>

namespace scopeone::ui
{
    CollapsibleSection::CollapsibleSection(const QString& title, QWidget* parent)
        : QFrame(parent)
        , m_title(title)
    {
        setFrameShape(QFrame::StyledPanel);

        m_header = new QToolButton(this);
        m_header->setCheckable(true);
        m_header->setChecked(true);
        m_header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_header->setArrowType(Qt::DownArrow);
        m_header->setAutoRaise(true);
        m_header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QFont font = m_header->font();
        font.setBold(true);
        m_header->setFont(font);
        m_header->setStyleSheet(QStringLiteral("QToolButton { text-align: left; padding: 3px; }"));

        m_body = new QWidget(this);
        m_form = new QFormLayout(m_body);
        m_form->setContentsMargins(8, 2, 8, 6);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(m_header);
        layout->addWidget(m_body);

        connect(m_header, &QToolButton::toggled, this, [this](bool expanded) { setExpanded(expanded); });
        updateTitle();
    }

    void CollapsibleSection::setSummary(const QString& summary)
    {
        if (summary == m_summary)
        {
            return;
        }
        m_summary = summary;
        updateTitle();
    }

    void CollapsibleSection::setExpanded(bool expanded)
    {
        m_body->setVisible(expanded);
        m_header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        if (m_header->isChecked() != expanded)
        {
            QSignalBlocker blocker(m_header);
            m_header->setChecked(expanded);
        }
        updateTitle();
    }

    bool CollapsibleSection::isExpanded() const
    {
        return m_header->isChecked();
    }

    // The summary shows beside the title while the block is folded
    void CollapsibleSection::updateTitle()
    {
        m_header->setText(m_summary.isEmpty() || isExpanded()
                              ? m_title
                              : QStringLiteral("%1 — %2").arg(m_title, m_summary));
    }
}

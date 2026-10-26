#include "WelcomeOverlay.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QToolButton>
#include <QIcon>
#include <QVBoxLayout>

namespace scopeone::ui
{
    WelcomeOverlay::WelcomeOverlay(QWidget* parent)
        : QWidget(parent, Qt::Dialog | Qt::FramelessWindowHint)
    {
        // A card of its own, a window over the main window that does not take the place of the viewer
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFixedWidth(440);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(24, 14, 24, 20);
        layout->setSpacing(10);

        auto* closeRow = new QHBoxLayout;
        closeRow->addStretch(1);
        auto* closeButton = new QToolButton(this);
        closeButton->setText(QStringLiteral("\u00d7"));
        closeButton->setAutoRaise(true);
        closeButton->setToolTip(tr("Close"));
        connect(closeButton, &QToolButton::clicked, this, &WelcomeOverlay::closeRequested);
        closeRow->addWidget(closeButton);
        layout->addLayout(closeRow);

        auto* logo = new QLabel(this);
        logo->setAlignment(Qt::AlignCenter);
        logo->setPixmap(QIcon(QStringLiteral(":/Scopeone_Icon.svg")).pixmap(QSize(80, 80)));
        layout->addWidget(logo);

        auto* title = new QLabel(tr("Welcome to ScopeOne"), this);
        title->setAlignment(Qt::AlignCenter);
        QFont titleFont = title->font();
        titleFont.setPointSizeF(titleFont.pointSizeF() * 1.5);
        titleFont.setBold(true);
        title->setFont(titleFont);
        layout->addWidget(title);

        auto* hint = new QLabel(tr("Load a configuration to start a camera,\nor open an image."), this);
        hint->setAlignment(Qt::AlignCenter);
        layout->addWidget(hint);

        auto* buttons = new QHBoxLayout;
        auto* openImage = new QPushButton(tr("Open Image..."), this);
        auto* loadConfiguration = new QPushButton(tr("Load Configuration..."), this);
        buttons->addStretch(1);
        buttons->addWidget(openImage);
        buttons->addWidget(loadConfiguration);
        buttons->addStretch(1);
        layout->addLayout(buttons);
        connect(openImage, &QPushButton::clicked, this, &WelcomeOverlay::openImageRequested);
        connect(loadConfiguration, &QPushButton::clicked, this, &WelcomeOverlay::loadConfigurationRequested);

        m_recentHost = new QWidget(this);
        m_recentLayout = new QVBoxLayout(m_recentHost);
        m_recentLayout->setContentsMargins(0, 6, 0, 0);
        m_recentLayout->setSpacing(2);
        layout->addWidget(m_recentHost);

        auto* tip = new QLabel(tr("Press L to search every command."), this);
        tip->setAlignment(Qt::AlignCenter);
        tip->setEnabled(false);
        layout->addWidget(tip);
    }

    void WelcomeOverlay::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().window());
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }

    void WelcomeOverlay::setRecent(const QStringList& images, const QStringList& configurations)
    {
        while (QLayoutItem* item = m_recentLayout->takeAt(0))
        {
            delete item->widget();
            delete item;
        }
        const auto addGroup = [this](const QString& heading, const QStringList& paths, bool image)
        {
            if (paths.isEmpty())
            {
                return;
            }
            auto* label = new QLabel(heading, m_recentHost);
            QFont font = label->font();
            font.setBold(true);
            label->setFont(font);
            m_recentLayout->addWidget(label);
            for (const QString& path : paths.mid(0, 4))
            {
                auto* link = new QPushButton(QFileInfo(path).fileName(), m_recentHost);
                link->setFlat(true);
                link->setStyleSheet(QStringLiteral("text-align: left; padding: 2px 4px;"));
                link->setToolTip(QDir::toNativeSeparators(path));
                link->setCursor(Qt::PointingHandCursor);
                connect(link, &QPushButton::clicked, this, [this, path, image]()
                {
                    if (image)
                    {
                        emit imageChosen(path);
                    }
                    else
                    {
                        emit configurationChosen(path);
                    }
                });
                m_recentLayout->addWidget(link);
            }
        };
        addGroup(tr("Recent images"), images, true);
        addGroup(tr("Recent configurations"), configurations, false);
    }
}

#include "WelcomeOverlay.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QIcon>
#include <QVBoxLayout>

namespace scopeone::ui
{
    WelcomeOverlay::WelcomeOverlay(QWidget* parent)
        : QWidget(parent)
    {
        setAutoFillBackground(true);
        auto* outer = new QVBoxLayout(this);
        outer->addStretch(1);

        auto* column = new QWidget(this);
        column->setMaximumWidth(460);
        auto* layout = new QVBoxLayout(column);
        layout->setSpacing(8);

        auto* logo = new QLabel(column);
        logo->setAlignment(Qt::AlignCenter);
        logo->setPixmap(QIcon(QStringLiteral(":/Scopeone_Icon.svg")).pixmap(QSize(96, 96)));
        layout->addWidget(logo);

        auto* title = new QLabel(tr("Welcome to ScopeOne"), column);
        title->setAlignment(Qt::AlignCenter);
        QFont titleFont = title->font();
        titleFont.setPointSizeF(titleFont.pointSizeF() * 1.6);
        titleFont.setBold(true);
        title->setFont(titleFont);
        layout->addWidget(title);

        auto* hint = new QLabel(tr("Load a hardware configuration to start a camera, or open an image."), column);
        hint->setAlignment(Qt::AlignCenter);
        hint->setWordWrap(true);
        hint->setEnabled(false);
        layout->addWidget(hint);

        auto* buttons = new QHBoxLayout;
        auto* openImage = new QPushButton(tr("Open Image..."), column);
        auto* loadConfiguration = new QPushButton(tr("Load Configuration..."), column);
        buttons->addStretch(1);
        buttons->addWidget(openImage);
        buttons->addWidget(loadConfiguration);
        buttons->addStretch(1);
        layout->addLayout(buttons);
        connect(openImage, &QPushButton::clicked, this, &WelcomeOverlay::openImageRequested);
        connect(loadConfiguration, &QPushButton::clicked, this, &WelcomeOverlay::loadConfigurationRequested);

        m_recentHost = new QWidget(column);
        m_recentLayout = new QVBoxLayout(m_recentHost);
        m_recentLayout->setContentsMargins(0, 12, 0, 0);
        layout->addWidget(m_recentHost);

        auto* tip = new QLabel(tr("Press L to search every command."), column);
        tip->setAlignment(Qt::AlignCenter);
        tip->setEnabled(false);
        layout->addWidget(tip);

        auto* row = new QHBoxLayout;
        row->addStretch(1);
        row->addWidget(column);
        row->addStretch(1);
        outer->addLayout(row);
        outer->addStretch(2);
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

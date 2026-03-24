#include "AboutDialog.h"
#include "AppVersion.h"
#include "scopeone/ScopeOneCore.h"

#include <QApplication>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>

namespace scopeone::ui
{
    AboutDialog::AboutDialog(QWidget* parent)
        : QDialog(parent)
    {
        setupUI();
    }

    void AboutDialog::setupUI()
    {
        // Build a simple about dialog with version info
        setWindowTitle("About ScopeOne");
        resize(520, 320);

        auto* mainLayout = new QVBoxLayout(this);
        auto* contentLayout = new QVBoxLayout();

        auto* logoLabel = new QLabel(this);
        logoLabel->setAlignment(Qt::AlignCenter);
        logoLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        const QPixmap logoPixmap(":/Scopeone_Logo.svg");
        if (!logoPixmap.isNull())
        {
            logoLabel->setPixmap(logoPixmap.scaledToWidth(320, Qt::SmoothTransformation));
        }
        mainLayout->addWidget(logoLabel, 0, Qt::AlignHCenter);

        m_contentLabel = new QLabel(this);
        m_contentLabel->setAlignment(Qt::AlignLeft);
        m_contentLabel->setWordWrap(true);
        m_contentLabel->setTextFormat(Qt::PlainText);
        setContent(SCOPEONE_APP_NAME, SCOPEONE_APP_VERSION_STRING);
        contentLayout->addWidget(m_contentLabel);

        auto* okButton = new QPushButton("OK", this);
        connect(okButton, &QPushButton::clicked, this, &AboutDialog::onOkClicked);
        contentLayout->addWidget(okButton, 0, Qt::AlignRight);

        mainLayout->addLayout(contentLayout);
    }

    int AboutDialog::showAbout(QWidget* parent, const QString& appName, const QString& version)
    {
        AboutDialog dialog(parent);
        dialog.setContent(appName, version);
        return dialog.exec();
    }

    void AboutDialog::setContent(const QString& appName, const QString& version)
    {
        // Keep app and core version text in one place
        const QString description =
            "A microscope control software based on Micro-Manager Core. "
            "Supports dual-camera real-time preview and image processing.";
        const QString techStack =
            "- Qt 6\n"
            "- zlib\n"
            "- libtiff\n"
            "- OpenCV 4.x\n"
            "- Micro-Manager Core\n"
            "- C++20";
        const QString coreInfo =
            QString("Core: ScopeOneCore %1").arg(scopeone::core::ScopeOneCore::getVersion());
        const QString license =
            "Copyright (C) 2025-2026 ScopeOne Project\n"
            "Licensed under the MIT License";
        m_contentLabel->setText(QString("%1 %2\n%3\n\n%4\n\n%5\n\n%6")
            .arg(appName,
                 version,
                 coreInfo,
                 description,
                 techStack,
                 license));
    }

    void AboutDialog::onOkClicked()
    {
        accept();
    }
} // namespace scopeone::ui

#include "AboutDialog.h"
#include "AppVersion.h"
#include "scopeone/ScopeOneCore.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSysInfo>
#include <QTextBrowser>

namespace scopeone::ui
{
    // Create the about dialog shell
    AboutDialog::AboutDialog(QWidget* parent)
        : QDialog(parent)
    {
        setupUI();
    }

    // Build a simple about dialog with version info
    void AboutDialog::setupUI()
    {
        setWindowTitle("About ScopeOne");
        resize(600, 420);

        auto* mainLayout = new QVBoxLayout(this);
        mainLayout->setSpacing(14);

        auto* logoLabel = new QLabel(this);
        logoLabel->setAlignment(Qt::AlignCenter);
        logoLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        const QPixmap logoPixmap(":/Scopeone_Logo.svg");
        if (!logoPixmap.isNull())
        {
            logoLabel->setPixmap(logoPixmap.scaledToWidth(260, Qt::SmoothTransformation));
        }
        mainLayout->addWidget(logoLabel, 0, Qt::AlignHCenter);

        m_contentBrowser = new QTextBrowser(this);
        m_contentBrowser->setOpenExternalLinks(true);
        setContent();
        mainLayout->addWidget(m_contentBrowser, 1);

        auto* okButton = new QPushButton("Close", this);
        connect(okButton, &QPushButton::clicked, this, &QDialog::accept);
        mainLayout->addWidget(okButton, 0, Qt::AlignRight);
    }

    // Show the about dialog modally
    int AboutDialog::showAbout(QWidget* parent)
    {
        AboutDialog dialog(parent);
        return dialog.exec();
    }

    // Keep app and core version text in one place
    void AboutDialog::setContent()
    {
        const QString title = QStringLiteral(SCOPEONE_APP_NAME " " SCOPEONE_APP_VERSION_STRING);
        const QString coreVersion = scopeone::core::ScopeOneCore::getVersion();
        const QString mmCoreVersion = scopeone::core::ScopeOneCore::getMMCoreVersion();
        const QString openCvVersion = scopeone::core::ScopeOneCore::getOpenCVVersion();
        const QString libTiffVersion = scopeone::core::ScopeOneCore::getLibTiffVersion();
        const QString zlibVersion = scopeone::core::ScopeOneCore::getZlibVersion();
        const QString commit = QStringLiteral(SCOPEONE_GIT_COMMIT);
        const QString platformInfo = QString("%1, %2")
            .arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture());

        m_contentBrowser->setHtml(QString(R"(
<html>
<body style="font-family: sans-serif; font-size: 10pt;">
<h2>%1</h2>

<h3>Runtime</h3>
<table cellspacing="4" cellpadding="0">
<tr><td><b>Commit</b></td><td>%2</td></tr>
<tr><td><b>ScopeOneCore</b></td><td>%3</td></tr>
<tr><td><b>MMCore</b></td><td>%4</td></tr>
<tr><td><b>Qt</b></td><td>%5</td></tr>
<tr><td><b>OpenCV</b></td><td>%6</td></tr>
<tr><td><b>libtiff</b></td><td>%7</td></tr>
<tr><td><b>zlib</b></td><td>%8</td></tr>
<tr><td><b>Platform</b></td><td>%9</td></tr>
</table>

<h3>Links</h3>
<p>
Project: <a href="https://github.com/Experimental-Microscopy-Lab/ScopeOne">https://github.com/Experimental-Microscopy-Lab/ScopeOne</a><br>
Issues: <a href="https://github.com/Experimental-Microscopy-Lab/ScopeOne/issues">https://github.com/Experimental-Microscopy-Lab/ScopeOne/issues</a>
</p>

<h3>Copyright and License</h3>
<p>
Copyright (c) 2025-2026, Tianyi Zhao.<br>
Licensed under the BSD 3-Clause License.
</p>
</body>
</html>
)")
            .arg(title.toHtmlEscaped(),
                 commit.toHtmlEscaped(),
                 coreVersion.toHtmlEscaped(),
                 mmCoreVersion.toHtmlEscaped(),
                 QString::fromLatin1(qVersion()).toHtmlEscaped(),
                 openCvVersion.toHtmlEscaped(),
                 libTiffVersion.toHtmlEscaped(),
                 zlibVersion.toHtmlEscaped(),
                 platformInfo.toHtmlEscaped()));
    }

} // namespace scopeone::ui

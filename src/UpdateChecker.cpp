#include "UpdateChecker.h"

#include "AppVersion.h"

#include <QProcess>
#include <QRegularExpression>

namespace scopeone::ui
{
    namespace
    {
        // The page of the latest release redirects to the page of its tag. Reading the redirect
        // needs no account and does not count against the limit of the GitHub API, which is shared
        // by everyone behind the same address.
        constexpr auto kLatestReleaseUrl =
            "https://github.com/Experimental-Microscopy-Lab/ScopeOne/releases/latest";

        // The numbers of a version such as "v1.5.1012"
        QList<int> versionNumbers(const QString& version)
        {
            QList<int> numbers;
            static const QRegularExpression digits(QStringLiteral("\\d+"));
            auto matches = digits.globalMatch(version);
            while (matches.hasNext())
            {
                numbers.append(matches.next().captured().toInt());
            }
            return numbers;
        }
    }

    UpdateChecker::UpdateChecker(QObject* parent)
        : QObject(parent)
    {
    }

    bool UpdateChecker::isNewer(const QString& latest, const QString& current)
    {
        const QList<int> a = versionNumbers(latest);
        const QList<int> b = versionNumbers(current);
        if (a.isEmpty() || b.isEmpty())
        {
            return false;
        }
        const int count = qMax(a.size(), b.size());
        for (int index = 0; index < count; ++index)
        {
            const int left = index < a.size() ? a.at(index) : 0;
            const int right = index < b.size() ? b.at(index) : 0;
            if (left != right)
            {
                return left > right;
            }
        }
        return false;
    }

    void UpdateChecker::check()
    {
        if (m_process)
        {
            return;
        }
        m_process = new QProcess(this);
        connect(m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus)
        {
            handleFinished(exitCode);
        });
        connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error)
        {
            if (error == QProcess::FailedToStart && m_process)
            {
                m_process->deleteLater();
                m_process = nullptr;
                emit failed(tr("curl is not available, so the check for updates cannot run."));
            }
        });
        // The redirect is not followed. curl prints where it points to and nothing else.
        m_process->start(QStringLiteral("curl"),
                         {QStringLiteral("--silent"),
                          QStringLiteral("--show-error"),
                          QStringLiteral("--max-time"), QStringLiteral("15"),
                          QStringLiteral("-A"), QStringLiteral("ScopeOne/%1").arg(
                              QStringLiteral(SCOPEONE_APP_VERSION_STRING)),
                          QStringLiteral("-o"), QStringLiteral("-"),
                          QStringLiteral("-w"), QStringLiteral("%{redirect_url}"),
                          QString::fromLatin1(kLatestReleaseUrl)});
    }

    void UpdateChecker::handleFinished(int exitCode)
    {
        if (!m_process)
        {
            return;
        }
        const QByteArray output = m_process->readAllStandardOutput();
        const QString error = QString::fromLocal8Bit(m_process->readAllStandardError()).trimmed();
        m_process->deleteLater();
        m_process = nullptr;

        if (exitCode != 0)
        {
            emit failed(error.isEmpty() ? tr("The request failed (curl exit code %1).").arg(exitCode) : error);
            return;
        }
        // Where the latest release page redirects to, ending in the tag, such as .../releases/tag/v1.5.1012.
        // curl prints the page itself too when there is no redirect, so only the end of the output counts.
        const QString output_text = QString::fromUtf8(output).trimmed();
        static const QRegularExpression tagUrl(QStringLiteral("(https://github\\.com/[^\\s\"'<>]*/releases/tag/([^\\s/\"'<>?#]+))$"));
        const QRegularExpressionMatch match = tagUrl.match(output_text);
        if (!match.hasMatch())
        {
            emit failed(tr("GitHub did not point to a release."));
            return;
        }
        const QString tag = match.captured(2);
        const QString releasePage = match.captured(1);
        const QString current = QStringLiteral(SCOPEONE_APP_VERSION_STRING);
        if (isNewer(tag, current))
        {
            QString version = tag;
            if (version.startsWith(QLatin1Char('v')))
            {
                version.remove(0, 1);
            }
            emit updateAvailable(version, QUrl(releasePage));
        }
        else
        {
            emit upToDate(current);
        }
    }
}

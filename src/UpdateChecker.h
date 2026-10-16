#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

class QProcess;

namespace scopeone::ui
{
    // Asks GitHub for the latest release of ScopeOne and tells whether it is newer than the
    // running version. The request is made by curl, which Windows 10 and later, Linux and macOS
    // ship with, so the application does not need a TLS library of its own for one request.
    class UpdateChecker : public QObject
    {
        Q_OBJECT

    public:
        explicit UpdateChecker(QObject* parent = nullptr);

        // Starts a check. Does nothing while one is running.
        void check();
        bool isChecking() const { return m_process != nullptr; }

        // True when the version in latest, such as "v1.5.1020", is newer than current, such as
        // "1.5.1012". Versions are compared number by number.
        static bool isNewer(const QString& latest, const QString& current);

    signals:
        void updateAvailable(const QString& version, const QUrl& releaseUrl);
        void upToDate(const QString& version);
        void failed(const QString& message);

    private:
        void handleFinished(int exitCode);

        QProcess* m_process{nullptr};
    };
}

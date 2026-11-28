#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>

#include "scopeone/scopeone_core_export.h"

namespace scopeone::core
{
    // One message of the log, its time in UTC. level is DEBUG, INFO, WARNING or ERROR. source
    // names where it came from: scopeone for ScopeOne itself, driverhost:<driver> for a driver
    // process, driverhost:micro-manager/<camera> for the process of a Micro-Manager camera,
    // python:<module> for a Python processing module. Micro-Manager and its device adapters keep
    // their own CoreLog files beside ScopeOne.log.
    struct LogEntry
    {
        QDateTime time;
        QString level;
        QString source;
        QString message;
        // Counts the entries of the process from 1, in the order they were written
        quint64 sequence{0};
    };

    // The one log of the application. It takes the Qt messages of the process and the lines that
    // the driver processes and the Python modules print, keeps the latest of them for the console
    // and the Local API, and writes all of them to ScopeOne.log. The log is for people reading what
    // happened, not for programs: those follow the request responses, the events and the action
    // log. Whoever does a piece of work logs it: Core what it does itself, and the requester, the
    // GUI among them, how its own requests ended.
    class SCOPEONE_CORE_EXPORT Log : public QObject
    {
        Q_OBJECT

    public:
        static Log* instance();

        // Takes over the Qt messages of the process and starts ScopeOne.log in directory. The log
        // of the session before is kept as ScopeOne.previous.log, as it tells why that one ended.
        static void install(const QString& directory);
        // The directory of the log files, empty before install
        static QString directory();

        static void write(const QString& level, const QString& source, const QString& message);
        static QJsonObject toJson(const LogEntry& entry);

        // The entries at or after since and with a sequence above after
        QList<LogEntry> entries(const QDateTime& since = QDateTime(), quint64 after = 0) const;

    signals:
        // Emitted on the thread that wrote the entry
        void entryAdded(const scopeone::core::LogEntry& entry);

    private:
        Log() = default;
    };
}

Q_DECLARE_METATYPE(scopeone::core::LogEntry)

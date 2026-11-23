#include "scopeone/Log.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <deque>
#include <iterator>

namespace scopeone::core
{
    namespace
    {
        constexpr std::size_t kMaxEntries = 10000;

        QMutex g_mutex;
        std::deque<LogEntry> g_entries;
        QFile g_file;
        QString g_directory;
        QtMessageHandler g_previousHandler = nullptr;

        QString levelName(QtMsgType type)
        {
            switch (type)
            {
            case QtDebugMsg: return QStringLiteral("DEBUG");
            case QtInfoMsg: return QStringLiteral("INFO");
            case QtWarningMsg: return QStringLiteral("WARNING");
            case QtCriticalMsg:
            case QtFatalMsg: return QStringLiteral("ERROR");
            }
            return QStringLiteral("INFO");
        }

        void handleMessage(QtMsgType type, const QMessageLogContext& context, const QString& message)
        {
            Log::write(levelName(type), QStringLiteral("scopeone"), message);
            if (g_previousHandler)
            {
                g_previousHandler(type, context, message);
            }
        }
    }

    Log* Log::instance()
    {
        static Log* const log = []()
        {
            qRegisterMetaType<LogEntry>();
            return new Log();
        }();
        return log;
    }

    void Log::install(const QString& directory)
    {
        bool opened = false;
        {
            QMutexLocker locker(&g_mutex);
            g_directory = directory;
            QDir().mkpath(directory);
            const QString path = QDir(directory).filePath(QStringLiteral("ScopeOne.log"));
            const QString previous = QDir(directory).filePath(QStringLiteral("ScopeOne.previous.log"));
            QFile::remove(previous);
            QFile::rename(path, previous);
            g_file.setFileName(path);
            opened = g_file.open(QIODevice::WriteOnly | QIODevice::Text);
        }
        instance();
        g_previousHandler = qInstallMessageHandler(handleMessage);
        if (!opened)
        {
            qWarning().noquote() << QStringLiteral("Cannot write the log file %1: %2")
                                        .arg(QDir::toNativeSeparators(g_file.fileName()), g_file.errorString());
        }
    }

    QString Log::directory()
    {
        QMutexLocker locker(&g_mutex);
        return g_directory;
    }

    void Log::write(const QString& level, const QString& source, const QString& message)
    {
        const LogEntry entry{QDateTime::currentDateTimeUtc(), level, source, message};
        {
            QMutexLocker locker(&g_mutex);
            if (g_entries.size() >= kMaxEntries)
            {
                g_entries.pop_front();
            }
            g_entries.push_back(entry);
            if (g_file.isOpen())
            {
                // Flushed at once, so the lines before a crash are in the file
                g_file.write(QStringLiteral("%1 %2 [%3] %4\n")
                                 .arg(entry.time.toLocalTime().toString(Qt::ISODateWithMs), level, source, message)
                                 .toUtf8());
                g_file.flush();
            }
        }
        // Outside the lock, as a receiver that logs would otherwise wait on it forever
        emit instance()->entryAdded(entry);
    }

    QJsonObject Log::toJson(const LogEntry& entry)
    {
        QJsonObject object;
        object.insert(QStringLiteral("time"), entry.time.toString(Qt::ISODateWithMs));
        object.insert(QStringLiteral("level"), entry.level);
        object.insert(QStringLiteral("source"), entry.source);
        object.insert(QStringLiteral("message"), entry.message);
        return object;
    }

    QList<LogEntry> Log::entries(const QDateTime& since) const
    {
        QMutexLocker locker(&g_mutex);
        QList<LogEntry> result;
        std::copy_if(g_entries.cbegin(), g_entries.cend(), std::back_inserter(result),
                     [&](const LogEntry& entry) { return !since.isValid() || entry.time >= since; });
        return result;
    }
}

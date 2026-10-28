#pragma once

#include <QByteArray>
#include <QObject>

#ifdef Q_OS_WIN
#include <thread>
class QWinEventNotifier;
#else
class QSocketNotifier;
#endif

namespace scopeone::ui
{
    // A shell attached to a pseudo terminal: forkpty on Unix, ConPTY on Windows
    class PtyProcess : public QObject
    {
        Q_OBJECT

    public:
        explicit PtyProcess(QObject* parent = nullptr);
        ~PtyProcess() override;

        void start(int columns, int rows);
        void write(const QByteArray& data);
        void resize(int columns, int rows);

    signals:
        void readyRead(const QByteArray& data);
        void finished(int exitCode);

    private:
#ifdef Q_OS_WIN
        void onProcessExited();

        void* m_pseudoConsole{nullptr};
        void* m_inputWrite{nullptr};
        void* m_outputRead{nullptr};
        void* m_process{nullptr};
        std::thread m_reader;
        QWinEventNotifier* m_exitNotifier{nullptr};
#else
        void readAvailable();

        int m_masterFd{-1};
        int m_pid{-1};
        QSocketNotifier* m_notifier{nullptr};
#endif
    };
}

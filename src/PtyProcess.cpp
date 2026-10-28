#include "PtyProcess.h"

#include <QDir>
#include <QMetaObject>

#ifdef Q_OS_WIN
#include <QWinEventNotifier>
#include <string>
#include <vector>
#include <windows.h>
#else
#include <QByteArrayList>
#include <QSocketNotifier>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#ifdef Q_OS_MACOS
#include <util.h>
#else
#include <pty.h>
#endif
extern char** environ;
#endif

namespace scopeone::ui
{
    PtyProcess::PtyProcess(QObject* parent)
        : QObject(parent)
    {
    }

#ifdef Q_OS_WIN

    PtyProcess::~PtyProcess()
    {
        // Closing the pseudo console ends the client and breaks the output pipe
        if (m_pseudoConsole)
        {
            ClosePseudoConsole(static_cast<HPCON>(m_pseudoConsole));
        }
        if (m_reader.joinable())
        {
            m_reader.join();
        }
        if (m_inputWrite) CloseHandle(m_inputWrite);
        if (m_outputRead) CloseHandle(m_outputRead);
        if (m_process) CloseHandle(m_process);
    }

    // Create the ConPTY, start PowerShell inside it and read its output on a thread
    void PtyProcess::start(int columns, int rows)
    {
        HANDLE inputRead = nullptr;
        HANDLE inputWrite = nullptr;
        HANDLE outputRead = nullptr;
        HANDLE outputWrite = nullptr;
        CreatePipe(&inputRead, &inputWrite, nullptr, 0);
        CreatePipe(&outputRead, &outputWrite, nullptr, 0);

        HPCON pseudoConsole = nullptr;
        CreatePseudoConsole(COORD{static_cast<SHORT>(columns), static_cast<SHORT>(rows)},
                            inputRead, outputWrite, 0, &pseudoConsole);
        // ConPTY keeps its own duplicates of the far pipe ends
        CloseHandle(inputRead);
        CloseHandle(outputWrite);

        SIZE_T attributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        std::vector<char> attributeBuffer(attributeSize);
        STARTUPINFOEXW startupInfo{};
        startupInfo.StartupInfo.cb = sizeof(startupInfo);
        startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startupInfo.lpAttributeList =
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
        InitializeProcThreadAttributeList(startupInfo.lpAttributeList, 1, 0, &attributeSize);
        UpdateProcThreadAttribute(startupInfo.lpAttributeList, 0,
                                  PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                  pseudoConsole, sizeof(pseudoConsole), nullptr, nullptr);

        std::wstring commandLine = L"powershell.exe -NoLogo";
        const std::wstring workingDirectory =
            QDir::toNativeSeparators(QDir::homePath()).toStdWString();
        PROCESS_INFORMATION processInfo{};
        CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                       EXTENDED_STARTUPINFO_PRESENT, nullptr, workingDirectory.c_str(),
                       &startupInfo.StartupInfo, &processInfo);
        DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
        CloseHandle(processInfo.hThread);

        m_pseudoConsole = pseudoConsole;
        m_inputWrite = inputWrite;
        m_outputRead = outputRead;
        m_process = processInfo.hProcess;

        // ConPTY keeps the output pipe open after the shell exits, so watch the process itself
        m_exitNotifier = new QWinEventNotifier(processInfo.hProcess, this);
        connect(m_exitNotifier, &QWinEventNotifier::activated, this, &PtyProcess::onProcessExited);

        m_reader = std::thread([this, outputRead]()
        {
            char buffer[8192];
            DWORD count = 0;
            while (ReadFile(outputRead, buffer, sizeof(buffer), &count, nullptr) && count > 0)
            {
                QByteArray data(buffer, static_cast<qsizetype>(count));
                QMetaObject::invokeMethod(this, [this, data]() { emit readyRead(data); },
                                          Qt::QueuedConnection);
            }
        });
    }

    void PtyProcess::write(const QByteArray& data)
    {
        DWORD written = 0;
        WriteFile(m_inputWrite, data.constData(), static_cast<DWORD>(data.size()), &written, nullptr);
    }

    void PtyProcess::resize(int columns, int rows)
    {
        ResizePseudoConsole(static_cast<HPCON>(m_pseudoConsole),
                            COORD{static_cast<SHORT>(columns), static_cast<SHORT>(rows)});
    }

    void PtyProcess::onProcessExited()
    {
        m_exitNotifier->setEnabled(false);
        DWORD exitCode = 0;
        GetExitCodeProcess(m_process, &exitCode);
        emit finished(static_cast<int>(exitCode));
    }

#else

    PtyProcess::~PtyProcess()
    {
        if (m_pid > 0)
        {
            ::kill(m_pid, SIGHUP);
            ::close(m_masterFd);
            ::waitpid(m_pid, nullptr, WNOHANG);
        }
    }

    // Fork the login user's shell onto a new pseudo terminal
    void PtyProcess::start(int columns, int rows)
    {
        const QByteArray shell = qEnvironmentVariableIsEmpty("SHELL")
                                     ? QByteArray("/bin/bash")
                                     : qgetenv("SHELL");
        const QByteArray home = QDir::homePath().toLocal8Bit();

        // Everything the child needs is built before fork, because only exec is safe after it
        QByteArrayList environment;
        for (char** entry = environ; *entry; ++entry)
        {
            const QByteArray variable(*entry);
            if (!variable.startsWith("TERM=") && !variable.startsWith("COLORTERM="))
            {
                environment.append(variable);
            }
        }
        environment.append("TERM=xterm-256color");
        environment.append("COLORTERM=truecolor");
        std::vector<char*> envp;
        for (QByteArray& variable : environment)
        {
            envp.push_back(variable.data());
        }
        envp.push_back(nullptr);
        char* argv[] = {const_cast<char*>(shell.constData()), nullptr};

        winsize size{};
        size.ws_col = static_cast<unsigned short>(columns);
        size.ws_row = static_cast<unsigned short>(rows);
        m_pid = ::forkpty(&m_masterFd, nullptr, nullptr, &size);
        if (m_pid == 0)
        {
            [[maybe_unused]] const int changed = ::chdir(home.constData());
            ::execve(shell.constData(), argv, envp.data());
            ::_exit(127);
        }

        ::fcntl(m_masterFd, F_SETFL, ::fcntl(m_masterFd, F_GETFL) | O_NONBLOCK);
        m_notifier = new QSocketNotifier(m_masterFd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, &PtyProcess::readAvailable);
    }

    void PtyProcess::write(const QByteArray& data)
    {
        qsizetype offset = 0;
        while (offset < data.size())
        {
            const ssize_t count = ::write(m_masterFd, data.constData() + offset,
                                          static_cast<size_t>(data.size() - offset));
            if (count <= 0 && errno != EINTR)
            {
                return;
            }
            offset += qMax<ssize_t>(count, 0);
        }
    }

    void PtyProcess::resize(int columns, int rows)
    {
        winsize size{};
        size.ws_col = static_cast<unsigned short>(columns);
        size.ws_row = static_cast<unsigned short>(rows);
        ::ioctl(m_masterFd, TIOCSWINSZ, &size);
    }

    // Drain the master side, and reap the shell once the slave side closes
    void PtyProcess::readAvailable()
    {
        char buffer[65536];
        for (;;)
        {
            const ssize_t count = ::read(m_masterFd, buffer, sizeof(buffer));
            if (count > 0)
            {
                emit readyRead(QByteArray(buffer, static_cast<qsizetype>(count)));
                continue;
            }
            if (count < 0 && errno == EINTR)
            {
                continue;
            }
            if (count < 0 && errno == EAGAIN)
            {
                return;
            }
            break;
        }

        m_notifier->setEnabled(false);
        int status = 0;
        ::waitpid(m_pid, &status, 0);
        ::close(m_masterFd);
        m_masterFd = -1;
        m_pid = -1;
        emit finished(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }

#endif
}

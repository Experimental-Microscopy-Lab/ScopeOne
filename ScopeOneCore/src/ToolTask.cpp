#include "scopeone/ToolTask.h"

#include <QFutureWatcher>
#include <QtConcurrent>
#include <exception>
#include <utility>

namespace scopeone::ui
{
    struct ScopeOneToolTask::Impl
    {
        Work work;
        std::atomic_bool cancelRequested{false};
        bool running{false};
        QString error;
        QFutureWatcher<void> watcher;
    };

    ScopeOneToolTask::ScopeOneToolTask(Work work, QObject* parent)
        : QObject(parent), m_impl(std::make_unique<Impl>())
    {
        m_impl->work = std::move(work);
        connect(&m_impl->watcher, &QFutureWatcher<void>::finished, this, [this]()
        {
            m_impl->running = false;
            if (!m_impl->error.isEmpty())
            {
                emit failed(m_impl->error);
                return;
            }
            if (m_impl->cancelRequested)
            {
                emit canceled();
            }
            else
            {
                emit finished();
            }
        });
    }

    ScopeOneToolTask::~ScopeOneToolTask()
    {
        cancel();
        m_impl->watcher.waitForFinished();
    }

    void ScopeOneToolTask::start()
    {
        if (!m_impl->work || m_impl->running)
        {
            return;
        }
        m_impl->running = true;
        m_impl->cancelRequested = false;
        m_impl->error.clear();
        const Work work = m_impl->work;
        m_impl->watcher.setFuture(QtConcurrent::run([this, work]()
        {
            try
            {
                work(m_impl->cancelRequested, [this](int percent)
                {
                    emit progressChanged(qBound(0, percent, 100));
                });
            }
            catch (const std::exception& exception)
            {
                m_impl->error = QString::fromLocal8Bit(exception.what());
            }
            catch (...)
            {
                m_impl->error = QStringLiteral("Tool task failed");
            }
        }));
    }

    void ScopeOneToolTask::cancel()
    {
        m_impl->cancelRequested = true;
    }

}

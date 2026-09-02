#pragma once

#include "scopeone/scopeone_sdk_export.h"

#include <QObject>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>

namespace scopeone::ui
{
    class SCOPEONE_SDK_EXPORT ScopeOneToolTask final : public QObject
    {
        Q_OBJECT

    public:
        using Work = std::function<void(const std::atomic_bool&,
                                        const std::function<void(int)>&)>;

        explicit ScopeOneToolTask(Work work, QObject* parent = nullptr);
        ~ScopeOneToolTask() override;

        void start();
        void cancel();

    signals:
        void progressChanged(int percent);
        void finished();
        void canceled();
        void failed(const QString& message);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}

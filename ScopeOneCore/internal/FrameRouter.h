#pragma once

#include <QObject>

#include "scopeone/ImageFrame.h"

namespace scopeone::core
{
    class ClockService;
}

namespace scopeone::core::internal
{
    class FrameRouter : public QObject
    {
        Q_OBJECT

    public:
        FrameRouter(scopeone::core::ClockService* clockService,
                    QObject* parent = nullptr);
        void publish(const scopeone::core::ImageFrame& frame);

    signals:
        void frameReady(const scopeone::core::ImageFrame& frame);

    private:
        scopeone::core::ClockService* m_clockService{nullptr};
    };
}

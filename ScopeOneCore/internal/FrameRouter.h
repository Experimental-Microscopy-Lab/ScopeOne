#pragma once

#include <QObject>

#include "scopeone/ImageFrame.h"

namespace scopeone::core::internal
{
    class FrameRouter : public QObject
    {
        Q_OBJECT

    public:
        explicit FrameRouter(QObject* parent = nullptr);
        void publish(const scopeone::core::ImageFrame& frame);

    signals:
        void frameReady(const scopeone::core::ImageFrame& frame);

    };
}

#pragma once

#include "scopeone/ImageFrame.h"
#include "scopeone/scopeone_sdk_export.h"

#include <QObject>
#include <QString>

namespace scopeone::ui
{
    class ScopeOneToolCore;

    class SCOPEONE_SDK_EXPORT ScopeOneToolFrameStream final : public QObject
    {
        Q_OBJECT

    public:
        explicit ScopeOneToolFrameStream(ScopeOneToolCore& core,
                                          QObject* parent = nullptr);

        void setSourceId(const QString& cameraId);

        void setEnabled(bool enabled);
        void setProcessing(bool processing);
        void clearPendingFrame();

    signals:
        void frameReady(const scopeone::core::ImageFrame& frame);

    private:
        void acceptFrame(const scopeone::core::ImageFrame& frame);

        QString m_sourceId;
        scopeone::core::ImageFrame m_pendingFrame;
        bool m_enabled{true};
        bool m_processing{false};
    };
}

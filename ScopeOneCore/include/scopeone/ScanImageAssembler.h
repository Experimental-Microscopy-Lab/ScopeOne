#pragma once

#include "scopeone/ImageFrame.h"
#include "scopeone/SignalSource.h"
#include "scopeone/scopeone_sdk_export.h"

#include <QList>
#include <QVector>

namespace scopeone::core
{
    class SCOPEONE_SDK_EXPORT ScanImageAssembler final
    {
    public:
        explicit ScanImageAssembler(const QString& sourceId,
                                    const ScanImageConfig& config);

        bool isValid() const;
        void reset();
        QList<ImageFrame> append(const TimestampedEventChunk& chunk);
        QList<ImageFrame> finish();

    private:
        void handleMarker(quint64 tick, quint32 code);
        void beginFrame();
        void beginLine(quint64 tick);
        void finishLine(quint64 tick);
        void finishFrame(quint64 tick);
        void appendEvent(quint64 tick);
        void emitFrame(const QVector<quint16>& pixels, quint64 tick);
        void emitAveragedFrame(quint64 tick);

        QString m_sourceId;
        ScanImageConfig m_config;
        QVector<quint16> m_framePixels;
        QVector<quint64> m_accumulatedPixels;
        QVector<quint64> m_lineEventTicks;
        QList<ImageFrame> m_readyFrames;
        int m_nextRow{0};
        int m_accumulatedFrameCount{0};
        bool m_frameActive{false};
        bool m_lineActive{false};
        quint64 m_lineStartTick{0};
        quint64 m_lastLineDurationTicks{0};
        quint64 m_lastFrameTick{0};
        double m_tickPeriodSeconds{0.0};
        quint64 m_nextFrameIndex{0};
    };
}

Q_DECLARE_METATYPE(scopeone::core::ScanImageConfig)

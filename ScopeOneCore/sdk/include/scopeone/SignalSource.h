#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QtGlobal>

#include <functional>

#include "scopeone/scopeone_sdk_export.h"

namespace scopeone::core
{
    enum class SignalSourceState
    {
        Idle,
        Starting,
        Running,
        Stopping,
        Error
    };

    enum class SignalStreamType
    {
        TimeSeries,
        TimestampedEvents
    };

    enum class SignalParameterType
    {
        Integer,
        Real,
        String,
        File,
        Choice
    };

    struct SignalParameterDescriptor
    {
        QString key;
        QString name;
        SignalParameterType type{SignalParameterType::String};
        QVariant defaultValue;
        bool hasRange{false};
        double minimum{0.0};
        double maximum{0.0};
        QString suffix;
        QVariantList choices;
        QStringList choiceNames;
        QString fileFilter;
    };

    struct SignalSourceDescriptor
    {
        QString id;
        QString name;
        QString provider;
        QString quantity;
        QString unit;
        SignalStreamType streamType{SignalStreamType::TimeSeries};
        QList<SignalParameterDescriptor> parameters;
    };

    struct ScanImageConfig
    {
        bool enabled{false};
        int width{256};
        int height{256};
        quint32 gain{1};
        int averageFrames{1};
        quint32 frameStartMarker{1};
        quint32 lineMarker{2};
        quint32 frameEndMarker{0};
        bool serpentine{false};
        bool mirrorHorizontal{false};
    };

    struct SignalAcquisitionConfig
    {
        QString sourceId;
        double sampleIntervalSeconds{0.01};
        int durationMs{360000000};
        bool publishTimestampedEvents{false};
        ScanImageConfig scanImage;
        QVariantMap sourceSettings;
    };

    struct SignalMarker
    {
        double timeSeconds{0.0};
        quint32 code{0};

        bool isValid() const
        {
            return qIsFinite(timeSeconds) && timeSeconds >= 0.0 && code != 0;
        }
    };

    struct TimeSeriesChunk
    {
        QString sourceId;
        QString quantity;
        QString unit;
        double startTimeSeconds{0.0};
        double sampleIntervalSeconds{0.0};
        QVector<double> values;
        QVector<SignalMarker> markers;
        quint64 totalInputEvents{0};
        quint64 totalMarkers{0};

        bool isValid() const
        {
            return !sourceId.isEmpty()
                && sampleIntervalSeconds > 0.0
                && !values.isEmpty();
        }
    };

    struct TimestampedEventChunk
    {
        QString sourceId;
        double tickPeriodSeconds{0.0};
        QVector<quint64> eventTicks;
        QVector<quint32> eventCodes;
        QVector<quint64> markerTicks;
        QVector<quint32> markerCodes;

        bool isValid() const
        {
            return !sourceId.isEmpty()
                && tickPeriodSeconds > 0.0
                && eventTicks.size() == eventCodes.size()
                && markerTicks.size() == markerCodes.size()
                && (!eventTicks.isEmpty() || !markerTicks.isEmpty());
        }
    };

    class SCOPEONE_SDK_EXPORT EventCountBinner
    {
    public:
        EventCountBinner(const QString& sourceId,
                         const QString& quantity,
                         const QString& unit,
                         double tickPeriodSeconds,
                         double sampleIntervalSeconds);

        void addEvent(quint64 tick);
        void addMarker(quint64 tick, quint32 code);
        void advanceToTick(quint64 tick);
        void advanceToElapsedSeconds(double elapsedSeconds);
        bool hasReadyChunks() const;
        QList<TimeSeriesChunk> takeReadyChunks();
        QList<TimeSeriesChunk> takeCompletedChunks();

    private:
        quint64 binForTick(quint64 tick) const;
        void appendCompletedValue(double value);
        void finishChunk();
        void advanceToBin(quint64 targetBin);

        QString m_sourceId;
        QString m_quantity;
        QString m_unit;
        double m_tickPeriodSeconds{0.0};
        quint64 m_ticksPerSample{1};
        quint64 m_currentSampleTick{0};
        quint64 m_nextSampleTick{1};
        double m_sampleIntervalSeconds{0.0};
        quint64 m_currentSample{0};
        quint64 m_firstCompletedSample{0};
        quint64 m_currentEventCount{0};
        quint64 m_totalInputEvents{0};
        quint64 m_totalMarkers{0};
        QVector<double> m_completedValues;
        QVector<SignalMarker> m_pendingMarkers;
        QList<TimeSeriesChunk> m_readyChunks;
    };

    // A device that puts out one value over time, such as a PMT, an APD or a
    // photon counter. It hands over the stream and the scan markers; ScopeOne plots the stream
    // and assembles scan images from it.
    class SCOPEONE_SDK_EXPORT SignalSource
    {
    public:
        using TimeSeriesSink = std::function<void(const TimeSeriesChunk&)>;
        using TimestampedEventSink = std::function<void(const TimestampedEventChunk&)>;
        using StateSink = std::function<void(const QString& sourceId,
                                             SignalSourceState state,
                                             const QString& message)>;

        virtual ~SignalSource();

        virtual QList<SignalSourceDescriptor> signalSources() const = 0;
        // The sinks may be called from any thread, and each in the order of the stream
        virtual void setSignalSinks(TimeSeriesSink timeSeries,
                                    TimestampedEventSink timestampedEvents,
                                    StateSink state) = 0;
        virtual bool startSignal(const SignalAcquisitionConfig& config,
                                 QString* errorMessage = nullptr) = 0;
        virtual void stopSignal(const QString& sourceId) = 0;
    };
}

Q_DECLARE_METATYPE(scopeone::core::SignalSourceState)
Q_DECLARE_METATYPE(scopeone::core::SignalMarker)
Q_DECLARE_METATYPE(scopeone::core::TimeSeriesChunk)
Q_DECLARE_METATYPE(scopeone::core::TimestampedEventChunk)

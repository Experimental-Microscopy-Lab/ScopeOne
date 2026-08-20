#pragma once

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QtGlobal>
#include <QtPlugin>

#include "scopeone/scopeone_core_export.h"

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
        quint32 frameStartMarker{1};
        quint32 lineStartMarker{2};
        quint32 lineEndMarker{0};
        quint32 frameEndMarker{0};
        bool serpentine{false};
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

    class SCOPEONE_CORE_EXPORT EventCountBinner
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

    class SCOPEONE_CORE_EXPORT SignalSource : public QObject
    {
        Q_OBJECT

    public:
        explicit SignalSource(QObject* parent = nullptr);
        ~SignalSource() override;

        virtual bool start(const SignalAcquisitionConfig& config,
                           QString* errorMessage = nullptr) = 0;
        virtual void stop() = 0;
        virtual SignalSourceState state() const = 0;
        virtual QString stateMessage() const = 0;

    signals:
        void timeSeriesReady(const scopeone::core::TimeSeriesChunk& chunk);
        void timestampedEventsReady(const scopeone::core::TimestampedEventChunk& chunk);
        void stateChanged(scopeone::core::SignalSourceState state,
                          const QString& message);
        void sourceError(const QString& errorMessage);
    };

    class SignalSourcePlugin
    {
    public:
        virtual ~SignalSourcePlugin() = default;
        virtual QList<SignalSourceDescriptor> signalSources() const = 0;
        virtual SignalSource* createSignalSource(const QString& sourceId,
                                                 QObject* parent = nullptr) = 0;
    };
}

#define SCOPEONE_SIGNAL_SOURCE_PLUGIN_IID "org.scopeone.SignalSourcePlugin/1.0"
Q_DECLARE_INTERFACE(scopeone::core::SignalSourcePlugin, SCOPEONE_SIGNAL_SOURCE_PLUGIN_IID)

Q_DECLARE_METATYPE(scopeone::core::SignalSourceState)
Q_DECLARE_METATYPE(scopeone::core::SignalMarker)
Q_DECLARE_METATYPE(scopeone::core::TimeSeriesChunk)
Q_DECLARE_METATYPE(scopeone::core::TimestampedEventChunk)

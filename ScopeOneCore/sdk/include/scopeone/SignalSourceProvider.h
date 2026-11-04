#pragma once

#include <QList>
#include <QString>

#include <functional>

#include "scopeone/SignalSource.h"
#include "scopeone/scopeone_sdk_export.h"

namespace scopeone::core
{
    // A hardware provider whose devices put out one value over time, such as a PMT, an APD or a
    // photon counter. It hands over the stream and the scan markers; ScopeOne plots the stream
    // and assembles scan images from it.
    class SCOPEONE_SDK_EXPORT SignalSourceProvider
    {
    public:
        using TimeSeriesSink = std::function<void(const TimeSeriesChunk&)>;
        using TimestampedEventSink = std::function<void(const TimestampedEventChunk&)>;
        using StateSink = std::function<void(const QString& sourceId,
                                             SignalSourceState state,
                                             const QString& message)>;

        virtual ~SignalSourceProvider();

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

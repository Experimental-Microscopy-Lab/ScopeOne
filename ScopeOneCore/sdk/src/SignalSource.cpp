#include "scopeone/SignalSource.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace scopeone::core
{
    EventCountBinner::EventCountBinner(const QString& sourceId,
                                       const QString& quantity,
                                       const QString& unit,
                                       double tickPeriodSeconds,
                                       double sampleIntervalSeconds)
        : m_sourceId(sourceId)
          , m_quantity(quantity)
          , m_unit(unit)
          , m_tickPeriodSeconds(tickPeriodSeconds)
          , m_ticksPerSample(std::max<quint64>(
                1,
                static_cast<quint64>(std::llround(
                    sampleIntervalSeconds / tickPeriodSeconds))))
          , m_nextSampleTick(m_ticksPerSample)
          , m_sampleIntervalSeconds(
                static_cast<double>(m_ticksPerSample) * tickPeriodSeconds)
    {
    }

    void EventCountBinner::addEvent(quint64 tick)
    {
        const quint64 bin = binForTick(tick);
        advanceToBin(bin);
        if (bin == m_currentSample)
        {
            ++m_currentEventCount;
            ++m_totalInputEvents;
        }
    }

    void EventCountBinner::addMarker(quint64 tick, quint32 code)
    {
        advanceToTick(tick);
        m_pendingMarkers.append({static_cast<double>(tick) * m_tickPeriodSeconds, code});
        ++m_totalMarkers;
    }

    void EventCountBinner::advanceToTick(quint64 tick)
    {
        advanceToBin(binForTick(tick));
    }

    void EventCountBinner::advanceToElapsedSeconds(double elapsedSeconds)
    {
        if (elapsedSeconds > 0.0)
        {
            advanceToBin(static_cast<quint64>(elapsedSeconds / m_sampleIntervalSeconds));
        }
    }

    bool EventCountBinner::hasReadyChunks() const
    {
        return !m_readyChunks.isEmpty();
    }

    QList<TimeSeriesChunk> EventCountBinner::takeReadyChunks()
    {
        return std::exchange(m_readyChunks, QList<TimeSeriesChunk>{});
    }

    QList<TimeSeriesChunk> EventCountBinner::takeCompletedChunks()
    {
        finishChunk();
        return takeReadyChunks();
    }

    void EventCountBinner::appendCompletedValue(double value)
    {
        constexpr qsizetype kMaximumChunkSamples = 65536;
        m_completedValues.append(value);
        if (m_completedValues.size() >= kMaximumChunkSamples)
        {
            finishChunk();
        }
    }

    void EventCountBinner::finishChunk()
    {
        if (m_completedValues.isEmpty())
        {
            return;
        }
        TimeSeriesChunk chunk;
        chunk.sourceId = m_sourceId;
        chunk.quantity = m_quantity;
        chunk.unit = m_unit;
        chunk.startTimeSeconds = static_cast<double>(m_firstCompletedSample)
            * m_sampleIntervalSeconds;
        chunk.sampleIntervalSeconds = m_sampleIntervalSeconds;
        chunk.values.swap(m_completedValues);
        const double chunkEndTime = static_cast<double>(
            m_firstCompletedSample + static_cast<quint64>(chunk.values.size()))
            * m_sampleIntervalSeconds;
        int markerCount = 0;
        while (markerCount < m_pendingMarkers.size()
               && m_pendingMarkers[markerCount].timeSeconds < chunkEndTime)
        {
            ++markerCount;
        }
        if (markerCount > 0)
        {
            chunk.markers = m_pendingMarkers.mid(0, markerCount);
            m_pendingMarkers.remove(0, markerCount);
        }
        chunk.totalInputEvents = m_totalInputEvents;
        chunk.totalMarkers = m_totalMarkers;
        m_firstCompletedSample += static_cast<quint64>(chunk.values.size());
        m_readyChunks.append(std::move(chunk));
    }

    quint64 EventCountBinner::binForTick(quint64 tick) const
    {
        if (tick >= m_currentSampleTick && tick < m_nextSampleTick)
        {
            return m_currentSample;
        }
        return tick / m_ticksPerSample;
    }

    void EventCountBinner::advanceToBin(quint64 targetBin)
    {
        if (targetBin <= m_currentSample)
        {
            return;
        }
        while (m_currentSample < targetBin)
        {
            appendCompletedValue(static_cast<double>(m_currentEventCount));
            m_currentEventCount = 0;
            ++m_currentSample;
        }
        m_currentSampleTick = m_currentSample * m_ticksPerSample;
        m_nextSampleTick = m_currentSampleTick + m_ticksPerSample;
    }
}

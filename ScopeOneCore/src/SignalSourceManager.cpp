#include "internal/SignalSourceManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QPluginLoader>

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
        m_pendingMarkers.append({static_cast<double>(tick) * m_tickPeriodSeconds,
                                 code});
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
            advanceToBin(static_cast<quint64>(
                elapsedSeconds / m_sampleIntervalSeconds));
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
        constexpr quint64 kMaximumExplicitEmptyBins = 65536;
        if (targetBin <= m_currentSample)
        {
            return;
        }
        if (targetBin - m_currentSample > kMaximumExplicitEmptyBins)
        {
            appendCompletedValue(static_cast<double>(m_currentEventCount));
            m_currentEventCount = 0;
            ++m_currentSample;
            finishChunk();
            m_currentSample = targetBin;
            m_firstCompletedSample = targetBin;
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

    SignalSource::SignalSource(QObject* parent)
        : QObject(parent)
    {
    }

    SignalSource::~SignalSource() = default;
}

namespace scopeone::core::internal
{
    SignalSourceManager::SignalSourceManager(QObject* parent)
        : QObject(parent)
    {
        loadPlugins();
    }

    SignalSourceManager::~SignalSourceManager()
    {
        for (const QPointer<SignalSource>& source : std::as_const(m_sources))
        {
            delete source.data();
        }
        m_sources.clear();
        m_loaders.clear();
    }

    QList<SignalSourceDescriptor> SignalSourceManager::sources() const
    {
        QList<SignalSourceDescriptor> result = m_descriptors.values();
        std::sort(result.begin(), result.end(),
                  [](const SignalSourceDescriptor& left,
                     const SignalSourceDescriptor& right)
                  {
                      return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
                  });
        return result;
    }

    bool SignalSourceManager::startTrace(const SignalAcquisitionConfig& config,
                                         QString* errorMessage)
    {
        const QString sourceId = config.sourceId.trimmed();
        if (sourceId.isEmpty() || !m_descriptors.contains(sourceId))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Unknown signal source: %1").arg(sourceId);
            }
            return false;
        }
        if (!std::isfinite(config.sampleIntervalSeconds)
            || config.sampleIntervalSeconds < 0.000001)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Signal sample interval must be at least 1 us");
            }
            return false;
        }
        if (config.durationMs < 1)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Signal acquisition duration must be positive");
            }
            return false;
        }

        SignalSource* source = sourceInstance(sourceId, errorMessage);
        return source && source->start(config, errorMessage);
    }

    void SignalSourceManager::stopTrace(const QString& sourceId)
    {
        if (SignalSource* source = m_sources.value(sourceId.trimmed()))
        {
            source->stop();
        }
    }

    SignalSourceState SignalSourceManager::state(const QString& sourceId) const
    {
        const QString id = sourceId.trimmed();
        if (const SignalSource* source = m_sources.value(id))
        {
            return source->state();
        }
        return m_states.value(id, SignalSourceState::Idle);
    }

    QString SignalSourceManager::stateMessage(const QString& sourceId) const
    {
        const QString id = sourceId.trimmed();
        if (const SignalSource* source = m_sources.value(id))
        {
            return source->stateMessage();
        }
        return m_messages.value(id, QStringLiteral("Signal source is idle"));
    }

    void SignalSourceManager::loadPlugins()
    {
        const QDir pluginDir(QDir(QCoreApplication::applicationDirPath())
                                 .filePath(QStringLiteral("plugins/signal-sources")));
        if (!pluginDir.exists())
        {
            return;
        }

        const QFileInfoList files = pluginDir.entryInfoList(QDir::Files);
        for (const QFileInfo& file : files)
        {
            if (!QLibrary::isLibrary(file.absoluteFilePath()))
            {
                continue;
            }
            auto loader = std::make_unique<QPluginLoader>(file.absoluteFilePath());
            QObject* instance = loader->instance();
            auto* plugin = qobject_cast<SignalSourcePlugin*>(instance);
            if (!plugin)
            {
                qWarning().noquote()
                    << QStringLiteral("Failed to load signal source plugin %1: %2")
                           .arg(file.fileName(), loader->errorString());
                continue;
            }

            for (const SignalSourceDescriptor& descriptor : plugin->signalSources())
            {
                const QString sourceId = descriptor.id.trimmed();
                if (sourceId.isEmpty() || m_descriptors.contains(sourceId))
                {
                    qWarning().noquote()
                        << QStringLiteral("Ignoring duplicate signal source ID: %1")
                               .arg(sourceId);
                    continue;
                }
                m_descriptors.insert(sourceId, descriptor);
                m_plugins.insert(sourceId, plugin);
                m_states.insert(sourceId, SignalSourceState::Idle);
                m_messages.insert(sourceId, QStringLiteral("Signal source is idle"));
            }
            m_loaders.push_back(std::move(loader));
        }
    }

    SignalSource* SignalSourceManager::sourceInstance(const QString& sourceId,
                                                      QString* errorMessage)
    {
        if (SignalSource* existing = m_sources.value(sourceId))
        {
            return existing;
        }
        SignalSourcePlugin* plugin = m_plugins.value(sourceId);
        SignalSource* source = plugin ? plugin->createSignalSource(sourceId, this) : nullptr;
        if (!source)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Failed to create signal source: %1").arg(sourceId);
            }
            return nullptr;
        }

        connect(source, &SignalSource::timeSeriesReady,
                this, &SignalSourceManager::timeSeriesReady);
        connect(source, &SignalSource::timestampedEventsReady,
                this, &SignalSourceManager::timestampedEventsReady);
        connect(source, &SignalSource::stateChanged,
                this, [this, sourceId](SignalSourceState state, const QString& message)
                {
                    m_states.insert(sourceId, state);
                    m_messages.insert(sourceId, message);
                    emit sourceStateChanged(sourceId, state, message);
                });
        connect(source, &SignalSource::sourceError,
                this, [this, sourceId](const QString& message)
                {
                    emit sourceError(sourceId, message);
                });
        m_sources.insert(sourceId, source);
        return source;
    }
}
